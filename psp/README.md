Я проанализирую архитектуру репозитория и дам рекомендации по портированию на PSP.Теперь соберу доказательства об архитектуре репозитория.Теперь изучу основную структуру исходного кода:Теперь у меня достаточно информации для подробного анализа. Создам полный ответ на русском языке.

## Обзор архитектуры Vector-06C SDL эмулятора

Это кросс-платформенный эмулятор советского компьютера Вектор-06Ц, написанный на C++ с поддержкой Linux, Windows, macOS, Raspberry Pi и Android. Проект уже имеет опыт портирования на мобильные платформы.

### Стек технологий
- **Язык:** C++ (78.9%), C (13.4%)
- **Framework:** SDL2 (основная версия), пользовательские абстракции для Android/Godot
- **Зависимости:** Boost (threading, filesystem), SDL2_image, ChaiScript (скриптинг), fast-filters (DSP обработка)
- **Сборка:** CMake

---

## Архитектура системы

```
src/
  main.cpp              Точка входа, инициализация компонентов
  emulator.cpp/h        Управление потоками, event loop (SDL vs native)
  board.cpp/h           Главный класс эмуляции - синхронизация компонентов
  
  === CPU ===
  i8080.cpp/h           Эмуляция процессора Intel 8080 (53К строк!)
  i8080_hal.h           Hardware abstraction layer для CPU
  
  === Память ===
  memory.cpp/h          64К адресного пространства, ROM/RAM управление
  
  === Видео ===
  tv.cpp/h              SDL2 window, OpenGL рендеринг, шейдеры
  filler.cpp/h          Генерация пикселей в реальном времени (bitmap emulation)
  shaders.cpp/h         GLSL шейдеры (singlepass.vsh/fsh)
  
  === Звук ===
  sound.cpp/h           Синтез аудио буферов, resampling
  soundnik.cpp/h        Высокоуровневый микс (timer + AY)
  resampler.cpp/h       Преобразование частоты семплирования
  
  === Периферия ===
  board.cpp             I/O контроллер, обработка портов 0-255
  vio.h                 Vector I/O - интерфейс портов
  8253.h                Intel 8253 Programmable Interval Timer
  ay.h                  AY-3-8910 (YM2149) - PSG синтезатор
  fd1793.h              Контроллер дисковода FD1793
  keyboard.h            Матрица клавиатуры (16×8 сканкоды)
  
  === Ввод-вывод ===
  options.cpp/h         Парсинг command-line аргументов
  
  === Отладка ===
  debug.cpp/h           Debugger API
  server.cpp/h          GDB server (port 4000, Z80/8080 support)
  
  === Скриптинг ===
  scriptnik.cpp/h       ChaiScript интеграция, hooks на frame/breakpoint
  
platform/
  android/              Android специфичный код
    event.h             SDL Event эмуляция через Android keycodes
    android_main.cpp    JNI точка входа
    emulator_android.cpp Single-threaded вариант
```

---

## Поток выполнения и взаимодействие компонентов

### Инициализация (main.cpp)
1. **Парсинг опций** → загрузка ROM/FDD/EDD файлов в Memory
2. **Инициализация периферии:**
   - `board.init()` — синхронизация тактирования
   - `filler.init()` — bitmap буферы
   - `soundnik.init()` — аудио девайс SDL
   - `tv.init()` — окно и шейдеры
   - `fdc.init()` — дисковод
3. **Загрузка ROM** → `memory.init_from_vector()`
4. **Запуск эмулятора** → `Emulator.start_emulator_thread()`

### Основной цикл эмуляции (emulator.cpp → board.cpp)

```
╔══════════════════════════╗
║  Event Loop (UI thread)  ║  ← SDL events (keyboard, mouse, quit)
╚═════────┬────────────────╝
          │ синхронизованная очередь
          ▼
╔══════════════════════════════════════╗
║  Emulator Thread                     ║
├──────────────────────────────────────┤
│  for each frame:                     │
│    CPU → cycles until VBLANK         │
│    ┌─ i8080_instruction()            │
│    │  • fetch, decode, execute       │
│    │  • memory read/write            │
│    │  • I/O port access (calls vio)  │
│    └─ check IRQ/INTE flags           │
│                                      │
│    Filler → scanline rendering       │
│    Soundnik → buffer frame audio     │
│    TV → display (SDL or OpenGL)      │
└──────────────────────────────────────┘
```

### Ключевые компоненты взаимодействия

**Board** — оркестратор:
- `execute_frame()` — цикл процессора до конца кадра
- `interrupt(bool)` — управление IRQ
- `render_frame()` — вызов TV рендера
- Hooks: `frame`, `jump` — для скриптов

**I/O (VIO)** — шина портов 0-255:
```cpp
IO io(memory, keyboard, timer, fdc, ay, tape_player);
```
- Порты 00-07 — видеопамять / режимы
- Порты 08-0F — клавиатура
- Порты 10-17 — контроллер дисковода (FD1793)
- Порты 18-1F — timer (8253)
- Порты 20-2F — AY синтезатор
- Порты 30-3F — tape/другое

**PixelFiller** — дешифратор видео:
- Читает видеопамять каждый цикл CPU
- Генерирует пиксели реального времени (640×288 pal)
- Подключает TV буфер через `tv.pixels()`

**Soundnik** — микшер:
- Accumulates samples от timer + AY
- Resampler преобразует в частоту девайса
- Callback подает в SDL audio queue

---

## Проблемы портирования на PSP и рекомендации

### 🔴 Критические проблемы

| Компонент | Проблема | Решение |
|-----------|----------|---------|
| **SDL2 зависимость** | PSP не поддерживает SDL2, нет OpenGL | Вырезать TV и заменить на PSP GU (графический юнит) |
| **Boost.Thread** | PSP лимитирован памятью (32 МБ), Boost тяжелый | Заменить на `pspthreadlib.h` (встроенный PSP pthread) |
| **ChaiScript** | Интерпретатор ~400 КБ кода, требует STL | Отключить скриптинг или вырезать из PSP версии |
| **Memory 64KB** | CPU требует 64К адресного пространства + буфер видео 16К | **Есть место**, но плотно. PSP RAM: 32 МБ хватит |
| **OpenGL шейдеры** | PSP не поддерживает GLSL | Переписать фиксированный пайплайн (GU API) или CPU-side rendering |

### 🟡 Средние приоритеты

| Компонент     | Адаптация                                          | Примечание                                 |
|---------------|----------------------------------------------------|--------------------------------------------|
| **Audio SDL** | Заменить на PSP Audio Library (`audio.h`)          | PSP поддерживает 44.1 kHz, не 96 kHz       |
| **Threading** | Переделать thread-safe queues через PSP semaphores | Уже есть пример в `emulator_android.cpp`   |
| **Keyboard**  | Парсить PSP джойстик вместо SDL                    | Маппинг кнопок на Вектор клавиатуру        |
| **File I/O**  | PSP использует fat: mount points                   | Использовать POSIX `fopen` (PSP совместим) |

### 🟢 Низкие приоритеты (копируются как есть)

- **i8080.cpp** — чистый CPU, нет платформы-зависимости
- **memory.cpp** — стандартный C++
- **board.cpp** — логика эмуляции, нужны минимальные правки
- **vio.h** — чистый code, I/O абстракция
- **filler.cpp** — генерация пикселей (нужна адаптация рендера)

---

## Адаптационный план для PSP

### Фаза 1: Минимальная версия (базовая эмуляция)
1. **Вырезать мультипоточность** → одно-потоковый вариант (как в Android)
   - Скопировать паттерн из `emulator_android.cpp`
   - Заменить Boost queues на простые переменные

2. **Вырезать SDL2**:
   - Заменить `tv.cpp` на PSP GU (Graphics Unit)
   - Вывод пиксельных буферов в VRAM
   - Простой 320×272 режим (половина разрешения Вектора)

3. **Заменить Boost.thread**:
   ```cpp
   #include <pspthreadlib.h>
   SceUID thid = sceKernelCreateThread("emulator", emulator_thread, 0x20, 256*1024, PSP_THREAD_ATTR_USER, NULL);
   ```

4. **Заменить audio**:
   ```cpp
   #include <pspaudiolib.h>
   pspAudioInit();
   pspAudioSetChannelCallback(0, audio_callback);
   ```

5. **Keyboard → PSP Controller**:
   ```cpp
   #include <pspctrl.h>
   SceCtrlData pad;
   sceCtrlReadBufferPositive(&pad, 1);
   // Маппинг: PSP buttons → Vector-06C keycodes
   ```

### Фаза 2: Оптимизация памяти
- Убрать ChaiScript (экономия ~500 КБ)
- Убрать SDL2_image (нет функции screenshot на PSP)
- Статические буферы вместо динамических
- Компиляция с `-mno-fused-madd` для ARM MIPS

### Фаза 3: Тестирование
- Загрузить ROM, проверить CPU cycles
- Проверить keyboard input
- Синхронизировать audio/video (50 Hz PAL)
- Профилирование на реальном PSP

---

## Файлы для первоочередной адаптации

**1. Системные обертки (создать новые):**
```
platform/psp/
  psp_main.cpp           Точка входа (main() → sceKernelStartThread)
  psp_tv.cpp/h           PSP GU вместо SDL
  psp_audio.cpp/h        PSP Audio API вместо SDL
  psp_controller.cpp/h   PSP Controller вместо keyboard.h
  psp_threading.cpp/h    PSP threading вместо Boost
```

**2. Модифицировать CMakeLists.txt:**
```cmake
if(PSP)
  set(EXTRA_DEFS "PSP=1")
  add_definitions(-D__PSP__)
  # PSPSDK flags
endif()
```

**3. Завернуть в условные компиляции:**
```cpp
// В emulator.h:
#if !defined(__ANDROID__) && !defined(__GODOT__) && !defined(__PSP__)
  #include "SDL.h"
  boost::thread thread;
#elif defined(__PSP__)
  #include <pspthreadlib.h>
  SceUID thread;
#endif
```

---

## Ожидаемые результаты

| Метрика | Значение |
|---------|----------|
| Рабочее разрешение | 320×240 (NTSC) или 320×272 (PAL) |
| CPU load | ~80-90% (PSP 333 MHz) |
| Audio | 44.1 kHz mono/stereo |
| Размер EBOOT | ~2-3 МБ |
| Требуемая RAM | ~8-12 МБ |

---

## Дополнительные ресурсы

- **PSPSDK документация:** https://github.com/pspdev/pspsdk
- **PSP GU примеры:** `pspsdk/samples/gu/`
- **Существующий port на мобильный:** `platform/android/` — уже показывает паттерны для non-SDL версии
- **GdbServer отключить:** Убрать `server.cpp` из PSP версии



## Полный справочник папок Vector-06C SDL эмулятора

### 📱 **android/** — Android приложение
Полнофункциональный Android port с UI на Java через JNI.
- `app/` — Gradle проект (структура типична для Android Studio)
- `build.gradle` — конфиг сборки, зависимости
- `gradle/`, `gradlew`, `gradlew.bat` — Gradle wrapper для кроссплатформной сборки
- `local.properties` — локальные пути (Android SDK, NDK)
- `platform/android/` — C++ код, специфичный для Android (JNI биндинги)
  - `android_main.cpp` — JNI функции для управления эмулятором из Java
  - `emulator_android.cpp` — однопоточный вариант эмулятора (для мобильных)
  - `event.h` — эмуляция SDL Event через Android keycodes
- **Статус:** ✅ Активен (готовый к использованию шаблон портирования)

---

### 🔧 **attic/** — Архивные/устаревшие компоненты
Код, который был заменен или оптимизирован. Хранится для истории и возможного восстановления.
- `biquad.cpp/h` — Биквадратичный фильтр для аудио (заменен на Resampler)
- **Статус:** ❌ Не используется в текущей сборке

---

### 🔤 **bas2txt/** — Конвертер BASIC файлов
Утилита для преобразования токенизированных BASIC 2.5 программ Вектора в текстовый формат и обратно.
- `bas2asc.py` — Python скрипт (13 КБ) для декомпиляции/компиляции .BAS файлов
  - Поддерживает двусторонний конверт: `.bas` ↔ `.asc` (ASCII)
  - Используется для редактирования программ в текстовом редакторе
- `test.cmd` — Примеры использования
- `testdata/` — Тестовые BASIC файлы
- **Статус:** ✅ Вспомогательный инструмент (не критичен для портирования)

---

### 📦 **boost/** — Boost C++ библиотека (версии)
Встроенная копия Boost (только заголовки, т.е. header-only версия).
- `boost/` — поддиректория с заголовками
  - Используется в main.cpp: `#include <boost/thread.hpp>`, `<boost/filesystem.hpp>`, etc.
- **Статус:** 🔴 **КРИТИЧНА ДЛЯ ИСКЛЮЧЕНИЯ на PSP!** Boost требует много памяти. Нужно заменить на native pthreads.

---

### 💾 **boot/** — ROM образы загрузчика
Бинарные образы постоянной памяти Вектора-06Ц (32 КБ каждый).
- `boots.bin` (2 КБ) — компактный загрузчик (используется по умолчанию)
  - Встраивается в исполняемый файл через objcopy (см. CMakeLists.txt)
- `boot.bin` (32 КБ) — полный загрузчик BASIC 2.5 TimSoft
  - Используется для загрузки BASIC программ
- `BOOT24.bin` (32 КБ) — альтернативный загрузчик (вероятно для Вектора-06Ц/1)
- `.txt` файлы — дизассемблированный исходный код (для документации)
- **Статус:** ✅ Критичны, встроены в бинарник, переносятся как есть

---

### 🔤 **chaiscript/** — ChaiScript интерпретатор
Встроенный скриптовый движок для автоматизации эмуляции (загрузка программ, отладка, hooking).
```cpp
// Пример из main.cpp:
scriptnik.append_from_file("scripts/bas25hook.chai");
board.hooks.frame = [&scriptnik](int frame) { scriptnik.onframe(frame); };
```
- `include/` — заголовки ChaiScript (~400+ КБ исходников)
- `LICENSE` — MIT
- **Статус:** 🟡 **Опциональный для PSP!** На мобильных платформах может быть отключен (снижение размера и памяти). Используется только для сложных скриптов типа `rkload.chai` или `basload.chai`.

---

### 🔨 **cmake/** — CMake модули
Вспомогательные скрипты для поиска зависимостей.
- `FindSDL2.cmake` — автоматический поиск SDL2 в системе
- `FindSDL2_image.cmake` — поиск SDL2_image (для сохранения PNG кадров)
- **Статус:** ✅ Стандартные, для PSP версии нужно создать `FindPSPSDK.cmake`

---

### 🛠️ **coreutil/** — Utility library
Универсальная библиотека утилит (собирается как static lib).
- `sources/coreutil/`
  - `cpu.cc` — утилиты для работы с CPU (вероятно инструменты отладки)
  - `memory.cc` — помощники для управления памятью
  - `ringbuffer.cc` — циклический буфер для аудио/видео
- `tools/` — инструменты разработки
- `cmake/` — модули CMake
- **Статус:** ✅ Платформо-независима, переносится на PSP как есть

---

### 🐘 **debian/** — Debian/Linux пакетирование
Конфиги для создания .deb пакета (для Ubuntu/Debian).
- `control` — зависимости пакета, описание
- `changelog` — история версий
- `compat` — уровень совместимости debhelper
- `rules` — правила сборки пакета
- `copyright` — лицензионная информация
- **Статус:** ℹ️ Только для Linux, не нужен для PSP

---

### 🎛️ **filters/** — Дизайн цифровых фильтров
Исследовательские материалы и генераторы FIR/IIR фильтров для audio resampling.
- `Resampling Filter Design.ipynb` — Jupyter notebook (SciPy + NumPy)
  - Дизайн интерполяционных фильтров (1.5 МГц → 44.1 кHz)
  - Экспортирует коэффициенты в C++ заголовки
- `halfband.py`, `shaderfir.py` — генераторы фильтров
- `interp.h`, `endstage.h`, `halfband.h` — готовые FIR коэффициенты
- `attic/` — старые варианты фильтров
- **Статус:** ✅ Не компилируется, только для документации/разработки. Готовые фильтры в .h используются в `resampler.cpp`

---

### 🎨 **res/** — Ресурсы (иконки, графика)
Графические активы приложения.
- `icon64.png` — исходная иконка (PNG, 64×64)
- `icon64.rgba` — бинарный формат (64×64×4 байта RGBA)
- `icon64.xcf` — исходник в GIMP
- `convert.bat` — Windows скрипт конверсии PNG → RGBA (для встраивания в бинарник)
- **Статус:** ✅ Встраиваются в бинарник через `objcopy`, переносятся на PSP

---

### 📜 **scripts/** — ChaiScript автоматизация
Скрипты для сложной загрузки программ, обхода ограничений эмулятора.

| Скрипт | Назначение |
|--------|-----------|
| `bas25hook.chai` | Hook для BASIC 2.5, перехватывает точки входа |
| `basload.chai` | Робот-типист: автоматически вводит команды загрузки .BAS |
| `robotnik.chai` | Общий автоматизатор (имитирует нажатия клавиш) |
| `rkload.chai` | Загрузка rk86 music system (3-уровневая загрузка) |
| `musload.chai` | Загрузка музыкальных файлов в rk86 |
| `iohook.chai` | Пример перехвата I/O портов для отладки |

**Пример использования:**
```bash
v06x --script bas25hook.chai --script robotnik.chai --bootrom boot.bin DIAMOND.BAS
```

- **Статус:** ✅ Опциональны для PSP (требуют ChaiScript). Можно отключить для уменьшения размера.

---

### 🔺 **shaders/** — GLSL шейдеры
GPU программы для видеорендера (OpenGL). Применяют визуальные эффекты к пикселям.
- `singlepass.vsh` — vertex shader (127 байт, минимален)
- `singlepass.fsh` — fragment shader (5 КБ)
  - Интерполяция, фильтрация, масштабирование видео
  - Поддерживает масштабирование с сохранением aspect ratio
- `defaults` — конфиг по умолчанию
- **Статус:** 🔴 **НЕ ПОРТИРУЕТСЯ НА PSP!** PSP не поддерживает GLSL. Нужна CPU-side растеризация или PSP GU (Graphics Unit) собственный язык ассемблера.

---

### ✅ **test/** — Модульные тесты
Набор тестов для валидации эмуляции и рендера видео.
- `tests.cpp` — C++ тесты (проверка корректности CPU, видео)
- `testsuite.sh` — главный скрипт запуска тестов
- `runtests-native.sh`, `runtests-wine.sh` — платформенные варианты
- `pngdiff.py` — сравнение PNG выходов (пиксельное тестирование)
- `expected/` — эталонные PNG изображения (expected output)
- **Статус:** ✅ Важны для валидации, пригодятся при отладке PSP версии

---

### 💾 **testroms/** — Тестовые ROM образы и дискеты
Банк тестовых программ для проверки корректности эмуляции.

| Файл | Тип | Назначение |
|------|-----|-----------|
| `testtp.rom` | ROM | Базовый тест загрузки |
| `test.fdd` | Дискета | Образ гибкого диска (840 КБ) с программами |
| `i8253.rom`, `i82531.rom` | ROM | Тесты таймера 8253 (разные режимы) |
| `chkvi53.rom` | ROM | Проверка таймера 8253 |
| `cpu_spd.rom`, `cpuspeed.rom` | ROM | Измерение скорости CPU |
| `scrltst2.rom` | ROM | Тест скролла видеопамяти |
| `clrs.rom` | ROM | Тест цветов + WAV для загрузки `clrs.wav` |
| `vst.rom` | ROM | Встроенный Вектор собственный тест |
| `.asm` файлы | Ассемблер | Исходники ROM образов |
| `.jpg` файлы | Скриншоты | Эталонные выходы на реальной машине (для сравнения) |

- **Статус:** ✅ Критичны для тестирования. Используются в `test/testsuite.sh`.

---

### 🎯 **src/** — Основной исходный код эмулятора
(Уже разобран в первом ответе, содержит ~40 файлов)

---

### 📁 **platform/** — Платформенные адаптации
- `android/` — код для Android (JNI, без SDL)
- `godot/` — адаптация для Godot Engine (опциональный port)
- **Статус:** Шаблон для PSP. Нужно создать `platform/psp/`

---

## Приоритеты для PSP портирования

| Папка | Статус | Действие |
|-------|--------|---------|
| `src/` | ✅ Ядро | Копировать с минимальными правками |
| `boot/` | ✅ ROM | Встроить как есть (2 КБ основного загрузчика) |
| `coreutil/` | ✅ Библиотека | Скомпилировать без изменений |
| `testroms/` | ✅ Тестирование | Включить `testtp.rom` и `test.fdd` для тестов |
| `test/` | ✅ Валидация | Портировать основной `tests.cpp` |
| `fast-filters/` | ✅ DSP | Скопировать (audio processing) |
| `filters/` | ℹ️ Только docs | Не нужен для сборки |
| `shaders/` | 🔴 Переделать | CPU-side или PSP GU вместо GLSL |
| `platform/android/` | 🟡 Образец | Использовать как шаблон для `platform/psp/` |
| `boost/` | 🔴 Заменить | Убрать, использовать PSP pthreads |
| `chaiscript/` | 🟡 Опционально | Отключить для уменьшения размера |
| `android/` | 🔵 Отдельный | Оставить как есть (не нужен для PSP) |
| `debian/` | 🔵 Отдельный | Оставить как есть (Linux специфичен) |