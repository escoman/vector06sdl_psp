# Схема работы эмулятора Vector-06C: Android версия

## 📋 Обзор архитектуры

Android версия использует **однопоточную** модель (отличие от desktop SDL версии). Все компоненты эмулятора работают в одном потоке, вызываемом из Java через JNI.

```
┌─────────────────────────────────────────┐
│     Java/Android UI Layer               │
│  (Kotlin/Java Activity)                 │
└──────────────┬──────────────────────────┘
               │ JNI Calls
               ▼
    ┌──────────────────────────┐
    │  android_main.cpp (JNI)  │
    │  Точка входа в C++       │
    └──────────────────────────┘
               │
        ┌──────┴─────────┐
        │                │
        ▼                ▼
    Init()          ExecuteFrame()
    ├─ Memory          ├─ Process input
    ├─ Board           ├─ Execute CPU cycle
    ├─ TV              ├─ Render pixels
    ├─ Sound           └─ Generate audio
    └─ FDC
```

---

## 🔧 Инициализация (android_main.cpp: Init)

### Статические глобальные объекты (строки 22-37)

```cpp
Memory memory;                          // 64K адресное пространство Вектора
FD1793 fdc;                             // Контроллер дисковода
Wav wav;                                // Загруженный WAV файл
WavPlayer tape_player(wav);             // Проигрыватель кассеты
Keyboard keyboard;                      // Матрица клавиатуры (16×8)
I8253 timer;                            // Intel 8253 таймер
AY ay;                                  // AY-3-8910 синтезатор
Soundnik soundnik(tw, aw);              // Микшер звука
IO io(...);                             // Шина портов 0-255
TV tv;                                  // Видеовывод
PixelFiller filler(memory, io, tv);     // Генератор пиксели в реальном времени
Board board(...);                       // Оркестратор эмуляции
Emulator lator(board);                  // Управитель (только для API)
```

**Ключевой момент:** Все объекты являются **статическими глобалями**, инициализируются один раз.

### Функция Init (строки 60-109)

```cpp
extern "C" JNIEXPORT jint JNICALL
Java_com_svofski_v06x_cpp_Emulator_Init(JNIEnv *env, jobject)
{
    // 1. Инициализация звука (опционально с записью)
    filler.init();           // Выделить пиксельный буфер
    soundnik.init(prec);     // Инициализировать audio backend
    tv.init();               // Создать видеоокно/поверхность
    board.init();            // Привязать компоненты через Board
    fdc.init();              // Инициализировать дисковод
    
    // 2. Конфигурация клавиатуры
    keyboard.onreset = [](bool blkvvod) {
        board.reset(blkvvod ? ResetMode::BLKVVOD : ResetMode::BLKSBR);
    };
    
    // 3. Софт-ресет с загрузкой bootloader
    board.reset(ResetMode::BLKVVOD);
    
    return 0xdeadbeef; // Готов к работе
}
```

**Состояние после Init:**
- ✅ Память инициализирована с загруженным bootloader
- ✅ CPU в состоянии RESET
- ✅ Видеобуфер готов
- ✅ Audio готов
- ❌ Эмуляция **НЕ запущена** (ждет Execute)

---

## 📥 Загрузка программ

### Функция LoadAsset (строки 167-184)

```cpp
extern "C" JNIEXPORT void JNICALL
Java_com_svofski_v06x_cpp_Emulator_LoadAsset(JNIEnv *env, jobject self,
    jbyteArray asset, jint kind, jint org)
{
    jsize size = env->GetArrayLength(asset);           // Получить размер
    jbyte * jbytes = env->GetByteArrayElements(...);   // Скопировать данные из Java
    
    switch(kind) {
        case LOADKIND.ROM:  // *.rom программа
        case LOADKIND.COM:  // *.com исполняемый файл
            load_rom((uint8_t*)jbytes, size, org);     // Загрузить по адресу org
            break;
        case LOADKIND.FDD:  // Образ гибкого диска
            load_fdd((uint8_t*)jbytes, size, org);     // Загрузить как диск org
            break;
        case LOADKIND.EDD:  // Расширенная память (cartridge)
            load_edd((uint8_t*)jbytes, size, org);     // Загрузить в слот 0x10000
            break;
    }
    env->ReleaseByteArrayElements(asset, jbytes, JNI_ABORT);
}
```

**Пример использования из Java:**
```java
// Загрузить ROM программу в 0xC000 (обычное место)
byte[] romBytes = readAsset("testtp.rom");
emulator.LoadAsset(romBytes, LoadKind.ROM, 0xC000);

// Загрузить дискету в drive 0
byte[] fddBytes = readAsset("test.fdd");
emulator.LoadAsset(fddBytes, LoadKind.FDD, 0);  // drive 0
```

**Адресное пространство памяти:**
```
0x0000-0x3FFF: Основная ОЗУ (16 КБ)
0x4000-0x7FFF: Video RAM + I/O (16 КБ)
0x8000-0xFFFF: Дополнительная ОЗУ (32 КБ)
0xC000-0xDFFF: ROM программа (загружается сюда)
0x10000+: Расширенная память (EDD, картриджи) на 64 КБ слот
```

---

## 🎬 Основной цикл эмуляции

### Функция ExecuteFrame (строки 130-151)

```cpp
extern "C" JNIEXPORT jint JNICALL
Java_com_svofski_v06x_cpp_Emulator_ExecuteFrame(JNIEnv *env, jobject self,
    jbyteArray pixels, jfloatArray samples)
{
    // 1️⃣  ВЫПОЛНИТЬ ОДИН КАДР
    lator.execute_frame();  // → emulator_android.cpp
    
    // 2️⃣  ЭКСПОРТИРОВАТЬ ПИКСЕЛИ В JAVA (VRAM → Java array)
    jbyte * jbytes = env->GetByteArrayElements(pixels, &isCopy);
    lator.export_pixel_bytes((uint8_t *)jbytes);
    env->ReleaseByteArrayElements(pixels, jbytes, JNI_COMMIT);
    
    // 3️⃣  ЭКСПОРТИРОВАТЬ ЗВУК В JAVA (Audio → Java array)
    jfloat * jfloats = env->GetFloatArrayElements(samples, &isCopy);
    jsize framesize = env->GetArrayLength(samples);
    lator.export_audio_frame(jfloats, framesize);
    env->ReleaseFloatArrayElements(samples, jfloats, JNI_COMMIT);
    
    return 0;
}
```

---

## ⚙️ Что происходит внутри execute_frame() (emulator_android.cpp)

### Функция Emulator::execute_frame (строки 13-35)

```cpp
void Emulator::execute_frame()
{
    // 📌 ЭТАП 1: Обработать накопленные нажатия клавиш
    for (int i = 0; i < N_SCANCODES; ++i) {
        if (this->keydowns[i]) {
            SDL_KeyboardEvent ev;
            ev.keysym.scancode = this->keydowns[i];
            board.handle_keydown(ev);    // Вызвать обработчик в Board
        }
        if (this->keyups[i]) {
            SDL_KeyboardEvent ev;
            ev.keysym.scancode = this->keyups[i];
            board.handle_keyup(ev);      // Вызвать обработчик в Board
        }
        this->keydowns[i] = this->keyups[i] = 0;  // Очистить буфер
    }
    
    // 📌 ЭТАП 2: Выполнить эмуляцию ОДНОГО кадра (50 Hz = 20 ms)
    int executed;
    if (Options.vsync && Options.vsync_enable) {
        // Синхронизировать кадры (пропускать некоторые)
        executed = board.execute_frame_with_cadence(true, true);
    } else {
        // Выполнить все кадры подряд
        executed = board.execute_frame_with_cadence(true, false);
    }
    
    // 📌 ЭТАП 3: (Опционально) Получить состояние джойстика
    // board.set_joysticks(joy_state_0, joy_state_1);
}
```

---

## 🎮 Ввод: клавиатура и джойстик

### Функции KeyDown/KeyUp (android_main.cpp, строки 153-163)

```cpp
extern "C" JNIEXPORT void JNICALL
Java_com_svofski_v06x_cpp_Emulator_KeyDown(JNIEnv *env, jobject self, jint scancode)
{
    lator.keydown((int)scancode);  // → Emulator::keydown()
}

extern "C" JNIEXPORT void JNICALL
Java_com_svofski_v06x_cpp_Emulator_KeyUp(JNIEnv *env, jobject self, jint scancode)
{
    lator.keyup((int)scancode);    // → Emulator::keyup()
}
```

### Функции Emulator::keydown/keyup (emulator_android.cpp, строки 52-67)

```cpp
void Emulator::keydown(int scancode)
{
    // Буфферизировать нажатие (выполнится на следующем execute_frame())
    for (int i = 0; i < N_SCANCODES; ++i) {
        if (this->keydowns[i] == 0 || this->keydowns[i] == scancode) {
            this->keydowns[i] = scancode;  // Запомнить нажатие
            break;
        }
    }
}

void Emulator::keyup(int scancode)
{
    // Буфферизировать отпускание
    for (int i = 0; i < N_SCANCODES; ++i) {
        if (this->keyups[i] == 0 || this->keyups[i] == scancode) {
            this->keyups[i] = scancode;    // Запомнить отпускание
        }
    }
}
```

**Ключевая архитектура:**
- Нажатия **не выполняются сразу**, а **буферизируются**
- Буффер очищается в начале `execute_frame()`
- Это гарантирует, что нажатия синхронизированы с циклом эмуляции

---

## 🧠 Основной цикл эмуляции внутри Board

### Функция Board::execute_frame_with_cadence (board.cpp, строки 174-178)

```cpp
int Board::execute_frame_with_cadence(bool update_screen, bool use_cadence)
{
    volatile bool c = cadence_allows();  // Проверить, выполнять ли этот кадр
    return (c || !use_cadence) && execute_frame(update_screen);
}
```

### Функция Board::execute_frame (board.cpp, строки 129-172)

Это **сердце эмулятора**:

```cpp
int Board::execute_frame(bool update_screen)
{
    // 🔔 Вызвать hook перед кадром (для скриптов)
    if (this->hooks.frame)
        this->hooks.frame(this->frame_no);
    
    // 🛑 Если debugger paused — пропустить эмуляцию
    if (this->debugger_interrupt || this->script_interrupt)
        return 0;
    
    ++this->frame_no;
    this->filler.reset();           // Сбросить буфер видео
    this->irq_carry = false;        // Очистить carry-over interrupt
    
    // 🔄 ОСНОВНОЙ ЦИКЛ: Эмулировать кадр до конца вертикальной развёртки
    while (!this->filler.brk) {  // filler.brk = конец экрана достигнут
        
        // 1️⃣  Проверить прерывание (VSYNC)
        this->check_interrupt();
        
        // 2️⃣  Проверить breakpoint для отладчика
        if (this->debugging && debug.check_break()) {
            this->debugger_interrupt = true;
            break;
        }
        
        // 3️⃣  Проверить скрипт-breakpoint
        if (this->scripting && check_breakpoint()) {
            this->script_interrupt = true;
            break;
        }
        
        // 4️⃣  ВЫПОЛНИТЬ ОДНУ ИНСТРУКЦИЮ И ОБНОВИТЬ ВИДЕО
        this->single_step(update_screen);
    }
    
    return 1;  // Кадр успешно выполнен
}
```

### Функция Board::single_step (board.cpp, строки 180-276)

Это самая **критичная функция**:

```cpp
void Board::single_step(bool update_screen)
{
    // 📌 ЭТАП 1: Выполнить одну инструкцию CPU 8080
    auto v_cycles = i8080_instruction(&this->last_opcode);
    total_v_cycles += v_cycles;
    this->instr_time += v_cycles;  // Накопить циклы
    
    // 📌 ЭТАП 2: Обновить видео в реальном времени
    // PixelFiller растеризует пиксели в буфер по мере выполнения CPU
    int afterbrk12 = this->filler.fill(
        this->instr_time << 2,      // Время в 12 МГц тактах
        commit_time,                 // Время write портов видео
        commit_time_pal,             // PAL коррекция
        update_screen);              // Нужно ли обновлять дисплей?
    
    // 📌 ЭТАП 3: Обработать прерывание от VSYNC (если оно сигнализировано)
    // Вектор-06Ц генерирует IRQ по окончании кадра (VSYNC сигнал)
    if (this->filler.irq) {
        int thresh = i8080_cycles();  // Количество тактов последней инструкции
        
        // Проверить, является ли прерывание валидным
        // (CPU может быть занята в конце длинной инструкции)
        if (this->filler.irq_clk > thresh * 4) {
            this->irq_carry = true;     // Отложить прерывание
        } else {
            this->irq |= this->inte && this->filler.irq;  // Установить флаг IRQ
        }
    }
    
    // 📌 ЭТАП 4: Обновить звук (для каждой инструкции)
    this->soundnik.soundSteps(
        this->instr_time / 2,        // Время в 750 kHz (1.5 МГц / 2)
        this->io.TapeOut(),          // Вывод кассеты
        this->io.Covox(),            // Синтезатор Covox
        this->tape_player.sample()); // Вход кассеты
    
    // 📌 ЭТАП 5: Обновить tape player
    if (this->frame_no > 60) {
        this->tape_player.advance(this->instr_time);
    }
    
    // 📌 ЭТАП 6: Обработать edge condition (если инструкция переполнила кадр)
    this->between += this->instr_time;
    this->instr_time = afterbrk12 >> 2;
    this->between -= this->instr_time;
}
```

**Временные диаграммы:**

```
Вектор-06Ц PAL режим: 50 Hz, 288 строк
────────────────────────────────────────────

CPU: 1.5 МГц
8080: 3 МГц (на Векторе переделано на 1.5 МГц)
Video: 12 МГц (внутренний клок филлера)

1 Кадр = 20 мс
├─ ~3000 инструкций CPU × (4-11 тактов/инстр)
├─ = ~59904 тактов CPU
└─ = ~239616 тактов 12 МГц (filler.fill использует это)

Audio:
├─ 1.5 МГц (CPU clock)
├─ Resampled → 44.1 kHz audio output
└─ ~882 samples за кадр (44100 / 50)
```

---

## 📺 Видеовывод (PixelFiller)

### Как растеризуется видео

```cpp
// В single_step():
int afterbrk12 = this->filler.fill(
    this->instr_time << 2,      // CPU циклы → 12 МГц циклы
    commit_time,                // Когда CPU писала в видеопорты
    commit_time_pal,
    update_screen);

// В filler.cpp:
// filler отслеживает все reads/writes в видеопамять
// И генерирует пиксели в реальном времени:
// - VRAM addr → read pixel data
// - Palette → convert 4-bit color → 32-bit RGBA
// - Render to TV::pixels() buffer
```

Когда выполняется:
```
1. CPU инструкция MOV A, M (читает видеопамять)
   ↓
2. PixelFiller.fill() видит адрес, время
   ↓
3. Растеризует соответствующий пиксель на текущую позицию экрана
   ↓
4. Обновляет TV framebuffer
```

**Результат:** Видео генерируется **в реальном времени** по мере выполнения CPU, что обеспечивает точное эмулирование аппаратной синхронизации.

---

## 🔊 Аудиовывод

### Функция export_audio_frame (emulator_android.cpp, строки 42-46)

```cpp
void Emulator::export_audio_frame(float * dst, size_t framesize)
{
    Soundnik * s = &board.get_soundnik();
    
    // Soundnik::callback генерирует аудиосэмплы
    // из буферов таймера (8253) и синтезатора (AY-3-8910)
    s->callback(s, (uint8_t *)dst, framesize * sizeof(float));
}
```

### Архитектура звука

```
CPU выполняет:
    └─ OUT portA, regvalue
        └─ Попадает в IO.onwrite()
            └─ 8253 Timer или AY обновляют состояние
                └─ Board::single_step() вызывает soundnik.soundSteps()
                    └─ Accumulate sample от timer + AY
                        └─ Resampler преобразует 1.5 МГц → 44.1 kHz
                            └─ Откладывает sample в circular buffer
                                └─ export_audio_frame() копирует в Java float[]
                                    └─ Java/Android Audio API воспроизводит
```

**Источники звука:**
1. **8253 Timer:** Прямоугольные волны (разные частоты)
2. **AY-3-8910:** PSG синтезатор (3 канала + noise)
3. **Кассета/Tape:** WAV playback
4. **Covox:** DAC (digital-to-analog converter)

---

## 💾 Сохранение/восстановление состояния

### Функции ExportState/RestoreState (android_main.cpp, строки 193-218)

```cpp
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_svofski_v06x_cpp_Emulator_ExportState(JNIEnv * env, jobject)
{
    std::vector<uint8_t> state;
    lator.save_state(state);  // Сериализовать ВСЕ состояние
    
    // Скопировать в Java byte array
    jbyteArray out_state = env->NewByteArray(state.size());
    jbyte * jbytes = env->GetByteArrayElements(out_state, NULL);
    std::copy(state.begin(), state.end(), jbytes);
    env->ReleaseByteArrayElements(out_state, jbytes, JNI_COMMIT);
    
    return out_state;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_svofski_v06x_cpp_Emulator_RestoreState(JNIEnv * env, jobject, jbyteArray in_state)
{
    jsize size = env->GetArrayLength(in_state);
    jbyte * jbytes = env->GetByteArrayElements(in_state, NULL);
    
    std::vector<uint8_t> state((uint8_t *)jbytes, (uint8_t *)jbytes + size);
    jboolean result = lator.restore_state(state);  // Десериализовать
    
    env->ReleaseByteArrayElements(in_state, jbytes, JNI_ABORT);
    return result;
}
```

### Что сохраняется (board.cpp, строки 694-701)

```cpp
void Board::serialize(std::vector<uint8_t>& to)
{
    this->memory.serialize(to);      // Вся 64K памяти
    this->io.serialize(to);          // Состояние всех портов I/O
    i8080cpu::serialize(to);         // Регистры и флаги CPU
    this->serialize_self(to);        // INTE, IRQ флаги Board
    this->debug.serialize(to);       // Отладочное состояние
}
```

**Размер:** ~70-100 KB (полное состояние машины)

---

## 📊 Полная диаграмма потока данных

```
┌─────────────────────────────────────────────────────────────────┐
│                        Java/Android UI                          │
├────────────┬─────────────────┬──────────────────┬───────────────┤
│   Init()   │ LoadAsset()    │ ExecuteFrame()   │ KeyDown()     │
└─────┬──────┴────────┬────────┴────────┬─────────┴───────┬──────┘
      │               │                │                 │
      ▼               ▼                ▼                 ▼
      │    ┌──────────────────────────────────┐          │
      │    │   android_main.cpp (JNI Layer)   │          │
      │    └──────────────────────────────────┘          │
      │                   │                               │
      ├─→ filler.init()   │                               │
      ├─→ soundnik.init() │                               │
      ├─→ tv.init()       │                               │
      ├─→ board.init()    │                               │
      └─→ board.reset()   │                               │
                          │                               │
      memory.init_from_vector()  (LoadAsset)             │
                          │                               │
                          ▼                               │
      ┌──────────────────────────────────────────┐        │
      │  Emulator::execute_frame()               │◄───────┘
      │  (emulator_android.cpp)                  │
      │  - Process keydown/keyup buffer          │
      │  - Call board.execute_frame_with_cadence│
      │  - Export pixels to Java                 │
      │  - Export audio to Java                  │
      └──────────────────────────────────────────┘
                          │
                          ▼
      ┌──────────────────────────────────────────┐
      │  Board::execute_frame()                  │
      │  (board.cpp)                             │
      │  - Loop while filler.brk == false        │
      │  - Call check_interrupt()                │
      │  - Call single_step() per instruction    │
      └──────────────────────────────────────────┘
                          │
                    ┌─────┴─────┬──────────┬──────────┐
                    │           │          │          │
                    ▼           ▼          ▼          ▼
            i8080_instruction()  │         │        │
            (i8080.cpp)          │         │        │
            - Fetch opcode       │         │        │
            - Decode             │         │        │
            - Execute            │         │        │
            - Update registers   │         │        │
            - Returns T-cycles   │         │        │
                    │            │         │        │
                    └────→ filler.fill()   │        │
                           (filler.cpp)   │        │
                           - Track VRAM   │        │
                           - Rasterize    │        │
                           - Return extra │
                           cycles to next └────→ soundnik.soundSteps()
                           frame                 (sound.cpp)
                                                 - Accumulate samples
                                                 - Resample
                                                 - Buffer to audio queue
                                                       │
                    ┌──────────────────────────────────┘
                    ▼
            board.get_tv().pixels()  (VRAM buffer)
            board.get_soundnik()     (Audio buffer)
                    │
                    └──→ JNI Export to Java
```

---

## 🔄 Временная последовательность выполнения (на примере одного фрейма)

```
T=0 ms:   Java вызывает ExecuteFrame()
         ↓
        JNI вызывает emulator_android.cpp::execute_frame()
        ├─ Проверить и обработать keydown/keyup
        └─ Вызвать board.execute_frame_with_cadence()
         ↓
T=0-20ms: Board::execute_frame() (основной цикл)
         WHILE (не конец видеокадра) DO:
         ├─ Check IRQ/VSYNC
         ├─ i8080_instruction() - Execute 1 CPU instr
         │  └─ ~4-11 ticks
         ├─ filler.fill() - Rasterize pixels
         │  └─ Каждый пиксель/строка по мере выполнения CPU
         ├─ soundnik.soundSteps() - Generate audio sample
         │  └─ Mix 8253 + AY + Tape
         └─ Loop repeat ~3000 раз
         ↓
T=20ms:   Кадр завершен (filler.brk == true)
         ↓
         Export pixels[] и samples[] в Java array
         ↓
         Вернуть управление Java коду
         ↓
T=20ms+:  Java::onFrameComplete()
         ├─ Отрисовать pixels[] на экран
         ├─ Воспроизвести samples[] в audio queue
         └─ (опционально) Вызвать ExecuteFrame() снова
```

---

## 🎯 Ключевые отличия Android версии от Desktop (SDL) версии

| Параметр | Desktop SDL | Android |
|----------|------------|---------|
| **Threading** | Multi-threaded (Boost threads + queues) | Single-threaded (JNI calls) |
| **Frame sync** | SDL event loop + timer callback | Java loop calls ExecuteFrame() |
| **Video output** | SDL2 window/OpenGL | Java canvas/SurfaceView |
| **Audio output** | SDL Audio callback | PSAudioLib (or Java AudioTrack) |
| **Input** | SDL KeyboardEvent loop | JNI KeyDown/KeyUp calls |
| **Control** | User controls timing | Java controls timing |
| **Scripting** | Full ChaiScript support | ChaiScript disabled |

---

## 🔌 Как портировать на PSP

Основываясь на Android архитектуре:

### 1️⃣ Заменить JNI на PSP equivalents

```cpp
// Вместо JNI (Java_com_svofski_...):
int psp_main(int argc, char *argv[]) {
    // Инициализация как в android_main.cpp::Init()
    filler.init();
    soundnik.init();
    tv.init();
    board.init();
    board.reset(ResetMode::BLKVVOD);
    
    // Основной цикл
    while(running) {
        // Обработать PSP controller input
        poll_psp_buttons();
        
        // Выполнить один фрейм
        lator.execute_frame();
        
        // Отрисовать пиксели на PSP GU
        render_to_psp_screen(board.get_tv().pixels());
        
        // Воспроизвести звук
        psp_audio_push(board.get_soundnik().buffer);
    }
}
```

### 2️⃣ Заменить компоненты платформы

| Компонент | SDL/Android | PSP |
|-----------|--------------|-----|
| TV (видео) | SDL2 + OpenGL | PSP GU |
| Soundnik | SDL Audio | PSP Audio Library |
| Keyboard | SDL Events | sceCtrlReadBufferPositive() |
| Threading | Boost/pthreads | psKernel threads |
| Main loop | Event loop | psp_main() + vsync |

### 3️⃣ Ядро остается **практически без изменений**

- `i8080.cpp` ✅ Копировать как есть
- `board.cpp` ✅ Копировать как есть (только удалить SDL includes)
- `emulator.cpp` ✅ Использовать логику `emulator_android.cpp`
- `memory.cpp` ✅ Копировать как есть
- `io.cpp` / VIO ✅ Копировать как есть
- `sound.cpp` ✅ Заменить только audio output backend

---

## 📝 Заключение

**Android версия демонстрирует идеальную архитектуру для портирования:**

✅ **Однопоточная модель** → легче портировать на PSP  
✅ **Четкое разделение** компонентов эмулятора и платформы  
✅ **Минимальные зависимости** от платформы (только видео/звук/ввод)  
✅ **Ядро абсолютно независимо** от платформы  

Для PSP нужно:
1. Создать `platform/psp/psp_main.cpp` (как `android_main.cpp`)
2. Создать `platform/psp/psp_tv.cpp` (рендер через GU)
3. Создать `platform/psp/psp_audio.cpp` (Audio Library)
4. Создать `platform/psp/psp_input.cpp` (Controller API)
5. Остальное скопировать из desktop версии!