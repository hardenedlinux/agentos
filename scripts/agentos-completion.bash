#
# Bash completion for agentos CLI.
#
# Drives entirely off `agentos <path> --complete` — no hardcoded command
# or option list. Adding a new subcommand or option in C++ automatically
# becomes tab-completable with no changes to this script.
#
# Install:
#   System-wide: /usr/share/bash-completion/completions/agentos
#   Per-user:    ~/.local/share/bash-completion/completions/agentos
#                or source it from ~/.bashrc
#
_agentos_complete() {
    local cur words cword
    _init_completion || return

    # Build the partial command path up to (not including) the current word.
    # Skip option-like tokens (starting with -); only positional subcommands
    # form the path passed to --complete.
    local cmd_path=()
    local i
    for ((i = 1; i < cword; i++)); do
        [[ "${words[i]}" != -* ]] && cmd_path+=("${words[i]}")
    done

    # Ask agentos what completions are available at this path.
    local completions
    completions=$(./build/agentos "${cmd_path[@]}" --complete 2>/dev/null)

    COMPREPLY=($(compgen -W "$completions" -- "$cur"))
}

complete -F _agentos_complete agentos
