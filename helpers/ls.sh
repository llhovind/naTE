unalias ls 2>/dev/null

ls() {
    cols=""

    if [ -n "$NATE_VPCOLUMNS_FILE" ] && [ -r "$NATE_VPCOLUMNS_FILE" ]; then
        cols=$(cat "$NATE_VPCOLUMNS_FILE")
    fi

    if [ -n "$cols" ]; then
        command ls --color=auto -w "$cols" "$@"
    else
        command ls --color=auto "$@"
    fi
}
