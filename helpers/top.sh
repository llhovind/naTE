top() {
    cleanup() {
        printf '\033[?1049l'
    }

    trap cleanup INT TERM

    printf '\033[?1049h'

    command top "$@"
    rc=$?

    cleanup
    trap - INT TERM

    return "$rc"
}
