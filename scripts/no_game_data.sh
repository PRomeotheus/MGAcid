# check_no_game_data <root> <what>
#
# Refuses a release that carries any part of the game. Shared by the Linux and
# Windows release scripts so the two can never drift: a check that exists in
# one of them and not the other is worse than no check, because it is trusted.
#
# Both the name and the content are looked at. A file renamed to hide what it
# is still has its header, and an ISO still has CD001 at offset 0x8001.
check_no_game_data() {
    local root="$1" what="$2" bad=()
    while IFS= read -r -d '' file; do
        local base lower
        base="$(basename "$file")"
        lower="${base,,}"
        case "$lower" in
            eboot*|*.iso|*.cso|*.pbp|*.prx|*.elf|*.ovl|*.bin|data.bin|param.sfo|umd_data*|ms0|savedata|ulus*|ulj*|npjb*)
                bad+=("$file (name)"); continue ;;
        esac
        [[ -f "$file" && ! -L "$file" ]] || continue
        local head
        head="$(head -c 20 "$file" | od -An -tx1 | tr -d ' \n')"
        case "$head" in
            7f454c46*)
                # ELF: e_machine at offset 18, little endian. 8 is MIPS, the PSP.
                [[ "${head:36:4}" == "0800" ]] && bad+=("$file (PSP executable)") ;;
            00504250*) bad+=("$file (PBP)") ;;
            7e505350*) bad+=("$file (encrypted PSP module)") ;;
            00505346*) bad+=("$file (PARAM.SFO)") ;;
        esac
        if [[ "$(stat -c %s "$file")" -gt 32774 ]] &&
           [[ "$(dd if="$file" bs=1 skip=32769 count=5 2>/dev/null | tr -d '\0')" == "CD001" ]]; then
            bad+=("$file (disc image)")
        fi
    done < <(find "$root" -print0)
    if [[ ${#bad[@]} -gt 0 ]]; then
        printf 'error: %s contains game data:\n' "$what" >&2
        printf '  %s\n' "${bad[@]}" >&2
        exit 1
    fi
    echo "no game data in $what"
}
