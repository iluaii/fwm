#!/bin/sh
# fwm-watch — непрерывный самописец сессии fwm.
#
# Предшественник (hangwatch) молчал, пока компози́тор отвечал по IPC, и писал
# снимок только когда тот замолкал совсем. Ровно этот случай он и пропустил:
# сессия после долгого простоя стала неюзабельной, но на `fwmctl version`
# отвечала — и в логе не осталось ничего, кроме "alive" раз в десять минут.
#
# Поэтому здесь пишется не «жив/мёртв», а КРИВАЯ. Раз в SAMPLE секунд одна
# строка CSV: сколько миллисекунд занял ответ по IPC, сколько процентов ядра
# съел компози́тор с прошлой пробы, сколько миллисекунд считала видеокарта,
# сколько памяти (RSS, VRAM, GTT), сколько дескрипторов и сколько из них
# dmabuf. Всё это читается без root.
#
# Что чем окажется, видно по форме кривой, а не по одной точке:
#   ipc_ms ползёт вверх, cpu_pct высокий  -> цикл событий чем-то забит
#   vram/gtt/dmabuf растут монотонно       -> течёт буфер, дальше своп и ад
#   rss растёт монотонно                   -> течёт CPU-сторона
#   всё ровно, а ipc_ms скакнул            -> встали в ядре (см. снимок)
#
# Порог ALERT_MS ловит «отвечает, но еле» — то, чего не хватило в прошлый раз:
# при его превышении рядом с обычной строкой кладётся полный снимок потоков.
#
# Запуск (из сессии fwm, не из-под root):
#   ~/fwm/tools/fwm-watch.sh &
# Смотреть:
#   column -s, -t ~/.local/state/fwm/watch.csv | tail -40
set -u

SAMPLE=${FWM_WATCH_SAMPLE:-30}    # секунд между пробами
ALERT_MS=${FWM_WATCH_ALERT_MS:-400}  # ответ дольше этого — снять слепок
STATE="${XDG_STATE_HOME:-$HOME/.local/state}/fwm"
CSV="$STATE/watch.csv"
LOG="$STATE/watch.log"

XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export XDG_RUNTIME_DIR WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
mkdir -p "$STATE"

# Один экземпляр на машину. Замок — pid-файл, а не pgrep: `pgrep -f fwm-watch`
# ловит и любую оболочку, у которой это имя оказалось в командной строке, и
# тогда сторож молча не запускается ровно там, где он нужен.
PIDFILE="$STATE/watch.pid"
if [ -f "$PIDFILE" ]; then
    _old=$(cat "$PIDFILE" 2>/dev/null)
    if [ -n "${_old:-}" ] && [ -d "/proc/$_old" ] &&
       grep -qs 'fwm-watch' "/proc/$_old/cmdline"; then
        exit 0    # уже следит
    fi
fi
echo $$ >"$PIDFILE"
trap 'rm -f "$PIDFILE"' EXIT INT TERM

say() { printf '%s %s\n' "$(date -Is)" "$*" >>"$LOG"; }

now_ms() { echo $(( $(date +%s%N) / 1000000 )); }

# Полный слепок: где стоит каждый поток, что с картой, кто ест процессор.
# /proc/PID/syscall и dmesg закрыты (ptrace_scope=1, dmesg_restrict=1), так что
# точной строки кода не будет — но wchan главного потока отвечает на главный
# вопрос: epoll (жив, ничего не делает) или ioctl в drm (встал на видеокарте).
snapshot() {
    _p=$1; _why=$2
    say "--- слепок: $_why ---"
    say "active_vt=$(cat /sys/class/tty/tty0/active 2>/dev/null) load=$(cut -d' ' -f1-3 /proc/loadavg)"
    if [ -n "$_p" ] && [ -d "/proc/$_p" ]; then
        for t in /proc/$_p/task/*; do
            say "  поток $(basename "$t") $(cat "$t/comm" 2>/dev/null) state=$(awk '{print $3}' "$t/stat" 2>/dev/null) wchan=$(cat "$t/wchan" 2>/dev/null)"
        done
    fi
    say "drm: $(for c in /sys/class/drm/card*-*/enabled; do printf '%s=%s ' "$(basename "$(dirname "$c")")" "$(cat "$c" 2>/dev/null)"; done)"
    say "dpms: $(for c in /sys/class/drm/card*-*/dpms; do printf '%s=%s ' "$(basename "$(dirname "$c")")" "$(cat "$c" 2>/dev/null)"; done)"
    say "asound: $(for f in /proc/asound/card*/pcm*p/sub*/status; do case "$(head -1 "$f" 2>/dev/null)" in *RUNNING*) printf '%s ' "$f";; esac; done)"
    say "top: $(ps -eo pid,stat,wchan:16,pcpu,rss,comm --sort=-pcpu 2>/dev/null | head -6 | tr '\n' '|')"
}

fwm_pid() { pgrep -x fwm 2>/dev/null | head -1; }

# Ноль вместо пустоты. /proc читается на живом процессе, и любая проба может
# застать его в момент, когда файла уже нет — пустая строка в арифметике
# уронила бы сторож ровно тогда, когда он нужнее всего.
num() { _v=$(cat 2>/dev/null); case "${_v:-}" in ''|*[!0-9]*) echo 0;; *) echo "$_v";; esac; }

# Сумма drm-total-{vram,gtt} по всем fd процесса, в КиБ. amdgpu отдаёт это в
# fdinfo без всяких прав; у одного процесса несколько drm-клиентов, поэтому
# складываем по всем и делим потом на глаз, а не претендуем на точность.
drm_kib() {
    _p=$1; _key=$2
    awk -v k="drm-total-$_key:" '
        $1 == k {
            v = $2
            if ($3 == "MiB") v *= 1024
            else if ($3 == "GiB") v *= 1048576
            else if ($3 == "B")   v /= 1024
            s += v
        }
        END { printf "%d", s }
    ' /proc/$_p/fdinfo/* 2>/dev/null | num
}

drm_gfx_ns() {
    awk '$1 == "drm-engine-gfx:" { s += $2 } END { printf "%d", s }' \
        /proc/$1/fdinfo/* 2>/dev/null | num
}

[ -f "$CSV" ] || echo "ts,ipc_ms,cpu_pct,gpu_ms,rss_mb,vram_mb,gtt_mb,fds,dmabuf,kids,zomb,vt,load1" >"$CSV"
say "=== fwm-watch запущен: проба раз в ${SAMPLE}с, порог ответа ${ALERT_MS}мс ==="

prev_pid=""; prev_ticks=0; prev_gfx=0; prev_wall=0
HZ=$(getconf CLK_TCK 2>/dev/null || echo 100)
down=0

while :; do
    pid=$(fwm_pid)
    if [ -z "$pid" ]; then
        say "fwm не найден"
        sleep "$SAMPLE"
        continue
    fi

    t0=$(now_ms)
    if timeout 10 fwmctl version >/dev/null 2>&1; then
        ipc_ms=$(( $(now_ms) - t0 ))
        if [ "$down" -gt 0 ]; then say "*** ответил снова после ${down} проб молчания ***"; down=0; fi
    else
        ipc_ms=-1
        down=$((down + 1))
        snapshot "$pid" "нет ответа по IPC, проба $down"
    fi

    ticks=$(awk '{print $14 + $15}' /proc/$pid/stat 2>/dev/null | num)
    gfx=$(drm_gfx_ns "$pid")
    wall=$(date +%s)

    cpu_pct=0; gpu_ms=0
    if [ "$pid" = "$prev_pid" ] && [ "$prev_wall" -gt 0 ]; then
        span=$((wall - prev_wall))
        [ "$span" -gt 0 ] && cpu_pct=$(( (ticks - prev_ticks) * 100 / HZ / span ))
        gpu_ms=$(( (gfx - prev_gfx) / 1000000 ))
    fi
    prev_pid=$pid; prev_ticks=$ticks; prev_gfx=$gfx; prev_wall=$wall

    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$(date -Is)" \
        "$ipc_ms" "$cpu_pct" "$gpu_ms" \
        "$(( $(awk '/VmRSS/{print $2}' /proc/$pid/status 2>/dev/null | num) / 1024 ))" \
        "$(( $(drm_kib "$pid" vram) / 1024 ))" \
        "$(( $(drm_kib "$pid" gtt) / 1024 ))" \
        "$(ls /proc/$pid/fd 2>/dev/null | wc -l)" \
        "$(ls -l /proc/$pid/fd 2>/dev/null | grep -c dmabuf)" \
        "$(ps --ppid "$pid" -o stat= 2>/dev/null | wc -l)" \
        "$(ps --ppid "$pid" -o stat= 2>/dev/null | grep -c Z)" \
        "$(cat /sys/class/tty/tty0/active 2>/dev/null)" \
        "$(cut -d' ' -f1 /proc/loadavg)" \
        >>"$CSV"

    # «Отвечает, но еле» — то, что прошлый сторож не умел заметить вовсе.
    if [ "$ipc_ms" -ge "$ALERT_MS" ] 2>/dev/null; then
        snapshot "$pid" "ответ занял ${ipc_ms}мс (порог ${ALERT_MS})"
    fi

    sleep "$SAMPLE"
done
