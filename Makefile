CC      = gcc
CFLAGS  = -Wall -Wextra -g -MMD -MP
LDLIBS  = -lX11 -lm -lXft -lXext
TARGET  = fwm
BUILD   = build

SRC     = $(shell find src -name '*.c')

OBJ     = $(SRC:src/%.c=$(BUILD)/%.o)
DEP     = $(OBJ:.o=.d)
CFLAGS += $(shell pkg-config --cflags x11 xft freetype2)
LDFLAGS += $(shell pkg-config --libs x11 xft freetype2)

XEPHYR_DISPLAY = :1

.PHONY: all run stop clean

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) -o $(TARGET) $(LDLIBS)

$(BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET)
	@if ! xdpyinfo -display $(XEPHYR_DISPLAY) >/dev/null 2>&1; then \
		echo "Xephyr $(XEPHYR_DISPLAY)..."; \
		Xephyr $(XEPHYR_DISPLAY) -screen 1280x800 & \
		sleep 1; \
	else \
		echo "Xephyr $(XEPHYR_DISPLAY)"; \
	fi
	DISPLAY=$(XEPHYR_DISPLAY) ./$(TARGET)

stop:
	-pkill -f "Xephyr $(XEPHYR_DISPLAY)"

clean:
	rm -rf $(TARGET) $(BUILD)

-include $(DEP)