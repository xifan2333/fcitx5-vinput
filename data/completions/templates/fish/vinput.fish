# fish completion for vinput

# @BEGIN_LOCAL_ASR@
function __fish_vinput_installed_models
    vinput -j model list 2>/dev/null | string match -r '"id":\s*"([^"]+)"' | string replace -r '"id":\s*"([^"]+)"' '$1'
end
# @END_LOCAL_ASR@

function __fish_vinput_installed_scenes
    vinput -j scene list 2>/dev/null | string match -r '"id":\s*"([^"]+)"' | string replace -r '"id":\s*"([^"]+)"' '$1'
end

function __fish_vinput_installed_providers
    vinput -j provider list 2>/dev/null | string match -r '"id":\s*"([^"]+)"' | string replace -r '"id":\s*"([^"]+)"' '$1'
end

function __fish_vinput_installed_llm_providers
    vinput -j llm list 2>/dev/null | string match -r '"id":\s*"([^"]+)"' | string replace -r '"id":\s*"([^"]+)"' '$1'
end

function __fish_vinput_installed_adapters
    vinput -j adapter list 2>/dev/null | string match -r '"id":\s*"([^"]+)"' | string replace -r '"id":\s*"([^"]+)"' '$1'
end

# Disable default file completions
complete -c vinput -f

# Global options
complete -c vinput -s j -l json -d "Output in JSON format"
complete -c vinput -s h -l help -d "Print help message and exit"
complete -c vinput -s v -l version -d "Display program version and exit"

# Top-level commands
complete -c vinput -n "__fish_use_subcommand" -a init -d "Initialize default config and directories"
# @BEGIN_LOCAL_ASR@
complete -c vinput -n "__fish_use_subcommand" -a model -d "Manage offline local ASR models"
# @END_LOCAL_ASR@
complete -c vinput -n "__fish_use_subcommand" -a provider -d "Manage cloud ASR providers"
complete -c vinput -n "__fish_use_subcommand" -a llm -d "Manage LLM providers"
complete -c vinput -n "__fish_use_subcommand" -a adapter -d "Manage LLM adapters"
# @BEGIN_LOCAL_ASR@
complete -c vinput -n "__fish_use_subcommand" -a hotword -d "Manage hotword file"
# @END_LOCAL_ASR@
complete -c vinput -n "__fish_use_subcommand" -a device -d "Manage audio capture devices"
complete -c vinput -n "__fish_use_subcommand" -a scene -d "Manage recognition scenes"
complete -c vinput -n "__fish_use_subcommand" -a config -d "Read or write configuration values"
complete -c vinput -n "__fish_use_subcommand" -a daemon -d "Control daemon lifecycle"
complete -c vinput -n "__fish_use_subcommand" -a recording -d "Control voice recording"
complete -c vinput -n "__fish_use_subcommand" -a rec -d "Control voice recording (alias)"

# init
complete -c vinput -n "__fish_seen_subcommand_from init" -s f -l force -d "Overwrite existing config"

# @BEGIN_LOCAL_ASR@
# model
complete -c vinput -n "__fish_seen_subcommand_from model; and not __fish_seen_subcommand_from list ls add use remove rm info" -a "list ls" -d "List installed or remote models"
complete -c vinput -n "__fish_seen_subcommand_from model; and not __fish_seen_subcommand_from list ls add use remove rm info" -a add -d "Download and install model"
complete -c vinput -n "__fish_seen_subcommand_from model; and not __fish_seen_subcommand_from list ls add use remove rm info" -a use -d "Set active local model"
complete -c vinput -n "__fish_seen_subcommand_from model; and not __fish_seen_subcommand_from list ls add use remove rm info" -a "remove rm" -d "Uninstall local model"
complete -c vinput -n "__fish_seen_subcommand_from model; and not __fish_seen_subcommand_from list ls add use remove rm info" -a info -d "Show model details"
complete -c vinput -n "__fish_seen_subcommand_from model; and __fish_seen_subcommand_from list ls" -s a -l available -d "List remote models"
complete -c vinput -n "__fish_seen_subcommand_from model; and __fish_seen_subcommand_from use remove rm info" -a "(__fish_vinput_installed_models)"
# @END_LOCAL_ASR@

# provider
complete -c vinput -n "__fish_seen_subcommand_from provider; and not __fish_seen_subcommand_from list ls add use edit e remove rm" -a "list ls" -d "List configured or remote providers"
complete -c vinput -n "__fish_seen_subcommand_from provider; and not __fish_seen_subcommand_from list ls add use edit e remove rm" -a add -d "Install provider from registry"
complete -c vinput -n "__fish_seen_subcommand_from provider; and not __fish_seen_subcommand_from list ls add use edit e remove rm" -a use -d "Set active ASR provider"
complete -c vinput -n "__fish_seen_subcommand_from provider; and not __fish_seen_subcommand_from list ls add use edit e remove rm" -a "edit e" -d "Edit provider script in editor"
complete -c vinput -n "__fish_seen_subcommand_from provider; and not __fish_seen_subcommand_from list ls add use edit e remove rm" -a "remove rm" -d "Uninstall ASR provider"
complete -c vinput -n "__fish_seen_subcommand_from provider; and __fish_seen_subcommand_from list ls" -s a -l available -d "List remote providers"
complete -c vinput -n "__fish_seen_subcommand_from provider; and __fish_seen_subcommand_from use edit e remove rm" -a "(__fish_vinput_installed_providers)"

# llm
complete -c vinput -n "__fish_seen_subcommand_from llm; and not __fish_seen_subcommand_from list ls add edit e remove rm test" -a "list ls" -d "List configured LLM providers"
complete -c vinput -n "__fish_seen_subcommand_from llm; and not __fish_seen_subcommand_from list ls add edit e remove rm test" -a add -d "Add an LLM provider"
complete -c vinput -n "__fish_seen_subcommand_from llm; and not __fish_seen_subcommand_from list ls add edit e remove rm test" -a "edit e" -d "Edit an LLM provider"
complete -c vinput -n "__fish_seen_subcommand_from llm; and not __fish_seen_subcommand_from list ls add edit e remove rm test" -a "remove rm" -d "Remove an LLM provider"
complete -c vinput -n "__fish_seen_subcommand_from llm; and not __fish_seen_subcommand_from list ls add edit e remove rm test" -a test -d "Test LLM provider connectivity"
complete -c vinput -n "__fish_seen_subcommand_from llm; and __fish_seen_subcommand_from add edit e" -s u -l base-url -d "Base URL"
complete -c vinput -n "__fish_seen_subcommand_from llm; and __fish_seen_subcommand_from add edit e" -s k -l api-key -d "API key"
complete -c vinput -n "__fish_seen_subcommand_from llm; and __fish_seen_subcommand_from add edit e" -s e -l extra-body -d "Extra JSON body"
complete -c vinput -n "__fish_seen_subcommand_from llm; and __fish_seen_subcommand_from edit e remove rm test" -a "(__fish_vinput_installed_llm_providers)"

# adapter
complete -c vinput -n "__fish_seen_subcommand_from adapter; and not __fish_seen_subcommand_from list ls ps status add start stop restart enable disable" -a "list ls" -d "List local or remote adapters"
complete -c vinput -n "__fish_seen_subcommand_from adapter; and not __fish_seen_subcommand_from list ls ps status add start stop restart enable disable" -a ps -d "List adapter processes and status"
complete -c vinput -n "__fish_seen_subcommand_from adapter; and not __fish_seen_subcommand_from list ls ps status add start stop restart enable disable" -a status -d "Show adapter status"
complete -c vinput -n "__fish_seen_subcommand_from adapter; and not __fish_seen_subcommand_from list ls ps status add start stop restart enable disable" -a add -d "Install an adapter"
complete -c vinput -n "__fish_seen_subcommand_from adapter; and not __fish_seen_subcommand_from list ls ps status add start stop restart enable disable" -a start -d "Start an adapter process"
complete -c vinput -n "__fish_seen_subcommand_from adapter; and not __fish_seen_subcommand_from list ls ps status add start stop restart enable disable" -a stop -d "Stop an adapter process"
complete -c vinput -n "__fish_seen_subcommand_from adapter; and not __fish_seen_subcommand_from list ls ps status add start stop restart enable disable" -a restart -d "Restart an adapter process"
complete -c vinput -n "__fish_seen_subcommand_from adapter; and not __fish_seen_subcommand_from list ls ps status add start stop restart enable disable" -a enable -d "Enable adapter autostart with daemon"
complete -c vinput -n "__fish_seen_subcommand_from adapter; and not __fish_seen_subcommand_from list ls ps status add start stop restart enable disable" -a disable -d "Disable adapter autostart with daemon"
complete -c vinput -n "__fish_seen_subcommand_from adapter; and __fish_seen_subcommand_from list ls" -s a -l available -d "List remote adapters"
complete -c vinput -n "__fish_seen_subcommand_from adapter; and __fish_seen_subcommand_from start stop restart enable disable status" -a "(__fish_vinput_installed_adapters)"

# @BEGIN_LOCAL_ASR@
# hotword
complete -c vinput -n "__fish_seen_subcommand_from hotword; and not __fish_seen_subcommand_from get set clear edit e" -a get -d "Show configured hotword path"
complete -c vinput -n "__fish_seen_subcommand_from hotword; and not __fish_seen_subcommand_from get set clear edit e" -a set -d "Set hotword file path"
complete -c vinput -n "__fish_seen_subcommand_from hotword; and not __fish_seen_subcommand_from get set clear edit e" -a clear -d "Clear hotword file path"
complete -c vinput -n "__fish_seen_subcommand_from hotword; and not __fish_seen_subcommand_from get set clear edit e" -a "edit e" -d "Edit hotword file in editor"
complete -c vinput -n "__fish_seen_subcommand_from hotword; and __fish_seen_subcommand_from set" -F
# @END_LOCAL_ASR@

# device
complete -c vinput -n "__fish_seen_subcommand_from device; and not __fish_seen_subcommand_from list ls use" -a "list ls" -d "List available audio input devices"
complete -c vinput -n "__fish_seen_subcommand_from device; and not __fish_seen_subcommand_from list ls use" -a use -d "Set active capture device"

# scene
complete -c vinput -n "__fish_seen_subcommand_from scene; and not __fish_seen_subcommand_from list ls add edit e use remove rm" -a "list ls" -d "List all scenes"
complete -c vinput -n "__fish_seen_subcommand_from scene; and not __fish_seen_subcommand_from list ls add edit e use remove rm" -a add -d "Add a new scene"
complete -c vinput -n "__fish_seen_subcommand_from scene; and not __fish_seen_subcommand_from list ls add edit e use remove rm" -a "edit e" -d "Edit a scene"
complete -c vinput -n "__fish_seen_subcommand_from scene; and not __fish_seen_subcommand_from list ls add edit e use remove rm" -a use -d "Set active scene"
complete -c vinput -n "__fish_seen_subcommand_from scene; and not __fish_seen_subcommand_from list ls add edit e use remove rm" -a "remove rm" -d "Remove a scene"
complete -c vinput -n "__fish_seen_subcommand_from scene; and __fish_seen_subcommand_from use remove rm" -a "(__fish_vinput_installed_scenes)"
complete -c vinput -n "__fish_seen_subcommand_from scene; and __fish_seen_subcommand_from add edit e" -l id -d "Scene ID"
complete -c vinput -n "__fish_seen_subcommand_from scene; and __fish_seen_subcommand_from add edit e" -s l -l label -d "Display label"
complete -c vinput -n "__fish_seen_subcommand_from scene; and __fish_seen_subcommand_from add edit e" -s t -l prompt -d "LLM prompt"
complete -c vinput -n "__fish_seen_subcommand_from scene; and __fish_seen_subcommand_from add edit e" -s p -l provider -a "(__fish_vinput_installed_llm_providers)" -d "LLM provider ID"
complete -c vinput -n "__fish_seen_subcommand_from scene; and __fish_seen_subcommand_from add edit e" -s m -l model -d "LLM model ID"
complete -c vinput -n "__fish_seen_subcommand_from scene; and __fish_seen_subcommand_from add edit e" -s c -l count -d "Maximum number of LLM candidates"
complete -c vinput -n "__fish_seen_subcommand_from scene; and __fish_seen_subcommand_from add edit e" -l timeout -d "Request timeout in ms"
complete -c vinput -n "__fish_seen_subcommand_from scene; and __fish_seen_subcommand_from add edit e" -l context-lines -d "Context history lines"

# config
complete -c vinput -n "__fish_seen_subcommand_from config; and not __fish_seen_subcommand_from get set edit e" -a get -d "Get a config value by JSON Pointer"
complete -c vinput -n "__fish_seen_subcommand_from config; and not __fish_seen_subcommand_from get set edit e" -a set -d "Set a config value by JSON Pointer"
complete -c vinput -n "__fish_seen_subcommand_from config; and not __fish_seen_subcommand_from get set edit e" -a "edit e" -d "Open config in editor"
complete -c vinput -n "__fish_seen_subcommand_from config; and __fish_seen_subcommand_from edit e" -a "core fcitx"
complete -c vinput -n "__fish_seen_subcommand_from config; and __fish_seen_subcommand_from set" -s i -l stdin -d "Read value from stdin"

# daemon
complete -c vinput -n "__fish_seen_subcommand_from daemon; and not __fish_seen_subcommand_from status start stop restart log" -a status -d "Show daemon status"
complete -c vinput -n "__fish_seen_subcommand_from daemon; and not __fish_seen_subcommand_from status start stop restart log" -a start -d "Start daemon"
complete -c vinput -n "__fish_seen_subcommand_from daemon; and not __fish_seen_subcommand_from status start stop restart log" -a stop -d "Stop daemon"
complete -c vinput -n "__fish_seen_subcommand_from daemon; and not __fish_seen_subcommand_from status start stop restart log" -a restart -d "Restart daemon"
complete -c vinput -n "__fish_seen_subcommand_from daemon; and not __fish_seen_subcommand_from status start stop restart log" -a log -d "Show daemon logs"
complete -c vinput -n "__fish_seen_subcommand_from daemon; and __fish_seen_subcommand_from log" -s f -l follow -d "Follow log output"
complete -c vinput -n "__fish_seen_subcommand_from daemon; and __fish_seen_subcommand_from log" -s n -l lines -d "Number of log lines"

# recording
complete -c vinput -n "__fish_seen_subcommand_from recording rec; and not __fish_seen_subcommand_from start stop toggle" -a start -d "Start recording"
complete -c vinput -n "__fish_seen_subcommand_from recording rec; and not __fish_seen_subcommand_from start stop toggle" -a stop -d "Stop recording and recognize"
complete -c vinput -n "__fish_seen_subcommand_from recording rec; and not __fish_seen_subcommand_from start stop toggle" -a toggle -d "Toggle recording"
complete -c vinput -n "__fish_seen_subcommand_from recording rec; and __fish_seen_subcommand_from stop toggle" -s s -l scene -a "(__fish_vinput_installed_scenes)"
