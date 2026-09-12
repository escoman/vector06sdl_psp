# Local PPSSPP deployment configuration

PPSSPP_DIR = $(HOME)/snap/ppsspp-emu/common/.config/ppsspp/PSP/GAME/VECTOR06C

.PHONY: deploy deploy-debug deploy-release deploy-autoselect deploy-profile

## deploy: release build + deploy (default)
deploy: deploy-release

## deploy-debug: debug build (DEBUG_ENABLED=1, -O2) + deploy
deploy-debug:
	@$(MAKE) DEBUG_ENABLED=1
	@$(MAKE) DEPLOY_TYPE=deploy-debug _do_deploy

## deploy-release: release build (DEBUG_ENABLED=0, -O3) + deploy
deploy-release:
	@$(MAKE) DEBUG_ENABLED=0
	@$(MAKE) DEPLOY_TYPE=deploy-release _do_deploy

## deploy-autoselect: release + AUTOSELECT_ROM test hook + deploy
deploy-autoselect:
	@$(MAKE) DEBUG_ENABLED=0 EXTRA_DEFS=-DAUTOSELECT_ROM
	@$(MAKE) DEPLOY_TYPE=deploy-autoselect _do_deploy

## deploy-profile: profiler build (PROFILE=1, DEBUG_ENABLED=0, -O2, -pg) + deploy
deploy-profile:
	@$(MAKE) PROFILE=1 DEBUG_ENABLED=0
	@$(MAKE) DEPLOY_TYPE=deploy-profile _do_deploy

# Conditional copy to PPSSPP_DIR (shared by all deploy-* targets).
# Skipped silently when PPSSPP_DIR does not exist or boot/EBOOT.PBP was not built.
_do_deploy:
	@if [ -d "$(PPSSPP_DIR)" ] && [ -f boot/EBOOT.PBP ]; then \
		cp -f boot/EBOOT.PBP $(PPSSPP_DIR)/EBOOT.PBP && \
		echo "Deployed $(DEPLOY_TYPE) to $(PPSSPP_DIR)"; \
	fi
