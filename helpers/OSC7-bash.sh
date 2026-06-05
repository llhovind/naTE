if [ -n "$BASH_VERSION" ]; then
    PROMPT_COMMAND='printf "\033]7;file://%s%s\033\\" "$(hostname)" "${PWD}"'
    PS1='\u@\h:\w\$ '
else
    PS1='\u@\h:\w\$ $(printf "\033]7;file://%s%s\033\\" "$(hostname)" "${PWD}")'
fi