CC      = gcc
CFLAGS  = -Wall -Wextra -g -MMD -MP
LDLIBS  = -lX11 -lm
TARGET  = fwm
SRC     = src/main.c src/physics.c src/window.c src/wm.c
BUILD   = build
OBJ     = $(SRC:src/%.c=$(BUILD)/%.o)
DEP     = $(OBJ:.o=.d)

XEPHYR_DISPLAY = :1

.PHONY: all run stop clean

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) -o $(TARGET) $(LDLIBS)

$(BUILD)/%.o: src/%.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET)
	@if ! xdpyinfo -display $(XEPHYR_DISPLAY) >/dev/null 2>&1; then \
		echo "Поднимаю Xephyr на $(XEPHYR_DISPLAY)..."; \
		Xephyr $(XEPHYR_DISPLAY) -screen 1280x800 & \
		sleep 1; \
	else \
		echo "Xephyr на $(XEPHYR_DISPLAY) уже запущен."; \
	fi
	DISPLAY=$(XEPHYR_DISPLAY) ./$(TARGET)

stop:
	-pkill -f "Xephyr $(XEPHYR_DISPLAY)"

clean:
	rm -rf $(TARGET) $(BUILD)

-include $(DEP)
