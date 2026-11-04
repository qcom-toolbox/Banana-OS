# ── Banana OS (ports/neofetch/build.sh puts this before main) ──────────
# Banana OS has no /proc: the facts come from the system's sysinfo command
# ("key: value" lines). uname -s says "Banana OS", with a space, which
# neofetch's own cache_uname would split.
if [[ "$(uname -s 2>/dev/null)" == "Banana OS" ]]; then
    banana_info() {
        [[ $banana_cache ]] || banana_cache=$(sysinfo 2>/dev/null)
        local line
        while IFS= read -r line; do
            [[ ${line%%: *} == "$1" ]] && { printf '%s\n' "${line#*: }"; return 0; }
        done <<< "$banana_cache"
        return 1
    }

    cache_uname() {
        kernel_name="Banana OS"
        kernel_version=$(uname -r)
        kernel_machine=$(uname -m)
    }

    get_os() { os="Banana OS"; }

    get_distro() {
        [[ $distro ]] && return
        case $distro_shorthand in
            tiny) distro="Banana OS" ;;
            *)    distro="Banana OS $(banana_info version)" ;;
        esac
        [[ $os_arch == on ]] && distro+=" $kernel_machine"
    }

    get_model() { model=; }

    get_kernel() { kernel="Banana Kernel $kernel_version"; }

    get_uptime() {
        local s d h m
        s=$(banana_info uptime_s) || return
        d=$((s / 60 / 60 / 24)) h=$((s / 60 / 60 % 24)) m=$((s / 60 % 60))
        ((d == 1)) && d="$d day,"  || d="$d days,"
        ((h == 1)) && h="$h hour," || h="$h hours,"
        ((m == 1)) && m="$m min"   || m="$m mins"
        uptime="$d $h $m"
        uptime=${uptime#0 days, }
        uptime=${uptime#0 hours, }
        uptime=${uptime%,}
        case $uptime_shorthand in
            on)   uptime=${uptime/ mins/m}; uptime=${uptime/ min/m}; uptime=${uptime/ hours/h}
                  uptime=${uptime/ hour/h}; uptime=${uptime/ days/d}; uptime=${uptime/ day/d} ;;
            tiny) uptime=${uptime/ mins/m}; uptime=${uptime/ min/m}; uptime=${uptime/ hours/h}
                  uptime=${uptime/ hour/h}; uptime=${uptime/ days/d}; uptime=${uptime/ day/d}
                  uptime=${uptime//,} ;;
        esac
    }

    get_packages() {
        local n
        n=$(banana_info packages) && packages="$n (pkg)"
    }

    get_resolution() { resolution=$(banana_info resolution); }

    get_de() {
        [[ $(banana_info desktop) == on ]] && de="Banana Desktop"
        de_run=1
    }

    get_wm() {
        [[ $(banana_info desktop) == on ]] && wm="Banana WM"
        wm_run=1
    }

    get_wm_theme() { :; }
    get_theme()    { :; }
    get_icons()    { :; }
    get_term_font(){ :; }

    get_term() {
        local t
        t=$(tty 2>/dev/null)
        case $t in
            /dev/pts/*)  term="Banana Terminal" ;;
            /dev/ttyS*)  term="serial console" ;;
            /dev/tty*)   term="Banana console" ;;
            *)           term="${TERM_PROGRAM:-$TERM}" ;;
        esac
    }

    get_cpu() {
        cpu=$(banana_info cpu) || return
        local threads
        threads=$(banana_info threads)
        cpu=${cpu//(R)/}
        cpu=${cpu//(TM)/}
        cpu=${cpu//(tm)/}
        cpu=${cpu//CPU /}
        cpu=${cpu//Processor/}
        cpu=${cpu//Core /}
        cpu=${cpu//  / }
        cpu=${cpu# }
        [[ $cpu_cores != off && $threads ]] && cpu="${cpu/@/($threads) @}"
        [[ $cpu_cores != off && $threads && $cpu != *"($threads)"* ]] && cpu+=" ($threads)"
        [[ $cpu_brand == off ]] && cpu=${cpu#Intel }
        [[ $cpu_brand == off ]] && cpu=${cpu#AMD }
    }

    get_gpu() { gpu=$(banana_info gpu); }

    get_memory() {
        local used total
        used=$(banana_info memory_used_kb) || return
        total=$(banana_info memory_total_kb) || return
        case $memory_unit in
            gib) used=$(awk -v u="$used" 'BEGIN{printf "%.2f", u / 1048576}')
                 total=$(awk -v t="$total" 'BEGIN{printf "%.2f", t / 1048576}')
                 mem_label=GiB ;;
            kib) mem_label=KiB ;;
            *)   used=$((used / 1024)) total=$((total / 1024)); mem_label=MiB ;;
        esac
        memory="${used}${mem_label} / ${total}${mem_label}"
        [[ $memory_percent == on ]] && memory+=" ($((used * 100 / total))%)"
    }

    # the Banana OS logo: yellow, the stem brown (256-colour 130)
    get_distro_ascii() {
        set_colors 11 130
        read -rd '' ascii_data <<'BANANA'
${c2}                     _
                    ( )
${c1}                    | |
                   /  |
                  /   |
                 /    |
               ,'     /
             ,'      /
          _,'       /
     __,-'        ,'
 ,--'          _,'
 `-.____,,.--''
BANANA
        # (neofetch's own rule: --ascii_colors replaces the logo's)
        [[ ${ascii_colors[0]} != distro ]] && {
            color_text=off
            set_colors "${ascii_colors[@]}"
        }
    }
fi

