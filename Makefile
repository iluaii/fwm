CC      = gcc
CFLAGS  = -Wall -Wextra -g
LDLIBS  = -lX11
TARGET  = fwm
SRC     = src/main.c

XEPHYR_DISPLAY = :1

.PHONY: all run stop clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDLIBS)

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
	rm -f $(TARGET)