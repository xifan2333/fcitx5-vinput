# fish completion for vinput

function __fish_vinput_installed_models
    vinput -j model list 2>/dev/null | string match -r '"id":\s*"([^"]+)"' | string replace -r '"id":\s*"([^"]+)"' '$1'
end

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

function __fish_vinput_installed_triggers
    vinput -j trigger list 2>/dev/null | string match -r '"id":\s*"([^"]+)"' | string replace -r '"id":\s*"([^"]+)"' '$1'
end

function __fish_vinput_installed_receivers
    vinput -j receiver list 2>/dev/null | string match -r '"id":\s*"([^"]+)"' | string replace -r '"id":\s*"([^"]+)"' '$1'
end

function __fish_vinput_is_lite
    if not set -q _vinput_is_lite
        if vinput -v 2>/dev/null | string match -q '*lite*'
            set -g _vinput_is_lite 1
        else
            set -g _vinput_is_lite 0
        end
    end
    test "$_vinput_is_lite" = 1
end

# Disable default file completions
complete -c vinput -f

# Global options
complete -c vinput -s j -l json -d "Output in JSON format"
complete -c vinput -s h -l help -d "Print help message and exit"
complete -c vinput -s v -l version -d "Display program version and exit"

# Top-level commands
complete -c vinput -n "__fish_use_subcommand" -a daemon -d "Control daemon lifecycle"
complete -c vinput -n "__fish_use_subcommand" -a d -d "Control daemon lifecycle (alias)"
complete -c vinput -n "__fish_use_subcommand" -a recording -d "Control voice recording"
complete -c vinput -n "__fish_use_subcommand" -a rec -d "Control voice recording (alias)"
complete -c vinput -n "__fish_use_subcommand" -a config -d "Read or write configuration values"
complete -c vinput -n "__fish_use_subcommand" -a cfg -d "Read or write configuration values (alias)"
complete -c vinput -n "__fish_use_subcommand" -a device -d "Manage audio capture devices"
complete -c vinput -n "__fish_use_subcommand" -a dev -d "Manage audio capture devices (alias)"
complete -c vinput -n "__fish_use_subcommand" -a provider -d "Manage cloud ASR providers"
complete -c vinput -n "__fish_use_subcommand" -a prov -d "Manage cloud ASR providers (alias)"
complete -c vinput -n "__fish_use_subcommand" -a trigger -d "Manage voice triggers"
complete -c vinput -n "__fish_use_subcommand" -a trig -d "Manage voice triggers (alias)"
complete -c vinput -n "__fish_use_subcommand" -a receiver -d "Manage voice receivers"
complete -c vinput -n "__fish_use_subcommand" -a recv -d "Manage voice receivers (alias)"
complete -c vinput -n "__fish_use_subcommand" -a scene -d "Manage recognition scenes"
complete -c vinput -n "__fish_use_subcommand" -a adapter -d "Manage LLM adapters"
complete -c vinput -n "__fish_use_subcommand" -a llm -d "Manage LLM providers"
complete -c vinput -n "__fish_use_subcommand; and not __fish_vinput_is_lite" -a model -d "Manage offline local ASR models"
complete -c vinput -n "__fish_use_subcommand; and not __fish_vinput_is_lite" -a mod -d "Manage offline local ASR models (alias)"
complete -c vinput -n "__fish_use_subcommand; and not __fish_vinput_is_lite" -a hotword -d "Manage hotword file"

# model
complete -c vinput -n "not __fish_vinput_is_lite; and __fish_seen_subcommand_from model mod; and not __fish_seen_subcommand_from list ls add use remove rm info" -a "list ls" -d "List installed or remote models"
complete -c vinput -n "not __fish_vinput_is_lite; and __fish_seen_subcommand_from model mod; and not __fish_seen_subcommand_from list ls add use remove rm info" -a add -d "Download and install model"
complete -c vinput -n "not __fish_vinput_is_lite; and __fish_seen_subcommand_from model mod; and not __fish_seen_subcommand_from list ls add use remove rm info" -a use -d "Set active local model"
complete -c vinput -n "not __fish_vinput_is_lite; and __fish_seen_subcommand_from model mod; and not __fish_seen_subcommand_from list ls add use remove rm info" -a "remove rm" -d "Uninstall local model"
complete -c vinput -n "not __fish_vinput_is_lite; and __fish_seen_subcommand_from model mod; and not __fish_seen_subcommand_from list ls add use remove rm info" -a info -d "Show model details"
complete -c vinput -n "not __fish_vinput_is_lite; and __fish_seen_subcommand_from model mod; and __fish_seen_subcommand_from list ls" -s a -l available -d "List remote models"
complete -c vinput -n "not __fish_vinput_is_lite; and __fish_seen_subcommand_from model mod; and __fish_seen_subcommand_from use remove rm info" -a "(__fish_vinput_installed_models)"

# provider
complete -c vinput -n "__fish_seen_subcommand_from provider prov; and not __fish_seen_subcommand_from list ls add use edit e remove rm" -a "list ls" -d "List configured or remote providers"
complete -c vinput -n "__fish_seen_subcommand_from provider prov; and not __fish_seen_subcommand_from list ls add use edit e remove rm" -a add -d "Install provider from registry"
complete -c vinput -n "__fish_seen_subcommand_from provider prov; and not __fish_seen_subcommand_from list ls add use edit e remove rm" -a use -d "Set active ASR provider"
complete -c vinput -n "__fish_seen_subcommand_from provider prov; and not __fish_seen_subcommand_from list ls add use edit e remove rm" -a "edit e" -d "Edit provider script in editor"
complete -c vinput -n "__fish_seen_subcommand_from provider prov; and not __fish_seen_subcommand_from list ls add use edit e remove rm" -a "remove rm" -d "Uninstall ASR provider"
complete -c vinput -n "__fish_seen_subcommand_from provider prov; and __fish_seen_subcommand_from list ls" -s a -l available -d "List remote providers"
complete -c vinput -n "__fish_seen_subcommand_from provider prov; and __fish_seen_subcommand_from use edit e remove rm" -a "(__fish_vinput_installed_providers)"

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
complete -c vinput -n "__fish_seen_subcommand_from adapter; and not __fish_seen_subcommand_from list ls add remove rm update up edit e" -a "list ls" -d "List local or remote adapters"
complete -c vinput -n "__fish_seen_subcommand_from adapter; and not __fish_seen_subcommand_from list ls add remove rm update up edit e" -a add -d "Install an adapter"
complete -c vinput -n "__fish_seen_subcommand_from adapter; and not __fish_seen_subcommand_from list ls add remove rm update up edit e" -a "remove rm" -d "Remove an adapter"
complete -c vinput -n "__fish_seen_subcommand_from adapter; and not __fish_seen_subcommand_from list ls add remove rm update up edit e" -a "update up" -d "Update an adapter script"
complete -c vinput -n "__fish_seen_subcommand_from adapter; and not __fish_seen_subcommand_from list ls add remove rm update up edit e" -a "edit e" -d "Edit adapter script in editor"
complete -c vinput -n "__fish_seen_subcommand_from adapter; and __fish_seen_subcommand_from list ls" -s a -l available -d "List remote adapters"
complete -c vinput -n "__fish_seen_subcommand_from adapter; and __fish_seen_subcommand_from add" -s e -l env -r -d "Adapter env var as KEY=VALUE"
complete -c vinput -n "__fish_seen_subcommand_from adapter; and __fish_seen_subcommand_from update up" -s a -l all -d "Update all installed adapters"
complete -c vinput -n "__fish_seen_subcommand_from adapter; and __fish_seen_subcommand_from update up" -s f -l force -d "Force update even if up to date"
complete -c vinput -n "__fish_seen_subcommand_from adapter; and __fish_seen_subcommand_from remove rm update up edit e" -a "(__fish_vinput_installed_adapters)"

# hotword
complete -c vinput -n "not __fish_vinput_is_lite; and __fish_seen_subcommand_from hotword; and not __fish_seen_subcommand_from get set clear edit e" -a get -d "Show configured hotword path"
complete -c vinput -n "not __fish_vinput_is_lite; and __fish_seen_subcommand_from hotword; and not __fish_seen_subcommand_from get set clear edit e" -a set -d "Set hotword file path"
complete -c vinput -n "not __fish_vinput_is_lite; and __fish_seen_subcommand_from hotword; and not __fish_seen_subcommand_from get set clear edit e" -a clear -d "Clear hotword file path"
complete -c vinput -n "not __fish_vinput_is_lite; and __fish_seen_subcommand_from hotword; and not __fish_seen_subcommand_from get set clear edit e" -a "edit e" -d "Edit hotword file in editor"
complete -c vinput -n "not __fish_vinput_is_lite; and __fish_seen_subcommand_from hotword; and __fish_seen_subcommand_from set" -F

# device
complete -c vinput -n "__fish_seen_subcommand_from device dev; and not __fish_seen_subcommand_from list ls use" -a "list ls" -d "List available audio input devices"
complete -c vinput -n "__fish_seen_subcommand_from device dev; and not __fish_seen_subcommand_from list ls use" -a use -d "Set active capture device"

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
complete -c vinput -n "__fish_seen_subcommand_from scene; and __fish_seen_subcommand_from add edit e" -l raw-cand -a "true false" -d "Include raw ASR transcript in candidate options (true/false)"
complete -c vinput -n "__fish_seen_subcommand_from scene; and __fish_seen_subcommand_from add edit e" -l raw-prev -a "true false" -d "Preview raw ASR transcript in floating panel during postprocessing (true/false)"

# config
complete -c vinput -n "__fish_seen_subcommand_from config cfg; and not __fish_seen_subcommand_from get set edit e migrate init" -a get -d "Get a config value by JSON Pointer"
complete -c vinput -n "__fish_seen_subcommand_from config cfg; and not __fish_seen_subcommand_from get set edit e migrate init" -a set -d "Set a config value by JSON Pointer"
complete -c vinput -n "__fish_seen_subcommand_from config cfg; and not __fish_seen_subcommand_from get set edit e migrate init" -a "edit e" -d "Open config in editor"
complete -c vinput -n "__fish_seen_subcommand_from config cfg; and not __fish_seen_subcommand_from get set edit e migrate init" -a migrate -d "Migrate legacy configuration files to current format"
complete -c vinput -n "__fish_seen_subcommand_from config cfg; and not __fish_seen_subcommand_from get set edit e migrate init" -a init -d "Initialize default config and directories"
complete -c vinput -n "__fish_seen_subcommand_from config cfg; and __fish_seen_subcommand_from migrate" -s n -l dry-run -d "Preview migration changes without modifying files"
complete -c vinput -n "__fish_seen_subcommand_from config cfg; and __fish_seen_subcommand_from init" -s f -l force -d "Overwrite existing config"
complete -c vinput -n "__fish_seen_subcommand_from config cfg; and __fish_seen_subcommand_from edit e" -a "core fcitx"
complete -c vinput -n "__fish_seen_subcommand_from config cfg; and __fish_seen_subcommand_from set" -s i -l stdin -d "Read value from stdin"

# daemon
complete -c vinput -n "__fish_seen_subcommand_from daemon d; and not __fish_seen_subcommand_from status st start stop restart enable disable log pipe" -a "status st" -d "Show daemon status"
complete -c vinput -n "__fish_seen_subcommand_from daemon d; and not __fish_seen_subcommand_from status st start stop restart enable disable log pipe" -a start -d "Start daemon"
complete -c vinput -n "__fish_seen_subcommand_from daemon d; and not __fish_seen_subcommand_from status st start stop restart enable disable log pipe" -a stop -d "Stop daemon"
complete -c vinput -n "__fish_seen_subcommand_from daemon d; and not __fish_seen_subcommand_from status st start stop restart enable disable log pipe" -a restart -d "Restart daemon"
complete -c vinput -n "__fish_seen_subcommand_from daemon d; and not __fish_seen_subcommand_from status st start stop restart enable disable log pipe" -a enable -d "Enable daemon to start with system"
complete -c vinput -n "__fish_seen_subcommand_from daemon d; and not __fish_seen_subcommand_from status st start stop restart enable disable log pipe" -a disable -d "Disable daemon from starting with system"
complete -c vinput -n "__fish_seen_subcommand_from daemon d; and not __fish_seen_subcommand_from status st start stop restart enable disable log pipe" -a log -d "Show daemon logs"
complete -c vinput -n "__fish_seen_subcommand_from daemon d; and not __fish_seen_subcommand_from status st start stop restart enable disable log pipe" -a pipe -d "Stream audio from STDIN to daemon for transcription"
complete -c vinput -n "__fish_seen_subcommand_from daemon d; and __fish_seen_subcommand_from enable" -s f -l force -d "Force enable symlink creation"
complete -c vinput -n "__fish_seen_subcommand_from daemon d; and __fish_seen_subcommand_from log" -s f -l follow -d "Follow log output"
complete -c vinput -n "__fish_seen_subcommand_from daemon d; and __fish_seen_subcommand_from log" -s n -l lines -r -d "Number of log lines"
complete -c vinput -n "__fish_seen_subcommand_from daemon d; and __fish_seen_subcommand_from log" -s l -l level -r -a "trace debug info warn error" -d "Log level filter"
complete -c vinput -n "__fish_seen_subcommand_from daemon d; and __fish_seen_subcommand_from pipe" -s s -l scene -r -a "(__fish_vinput_installed_scenes)" -d "Scene ID"
complete -c vinput -n "__fish_seen_subcommand_from daemon d; and __fish_seen_subcommand_from pipe" -l ime -d "Commit recognized text to desktop IME input context"

# trigger
complete -c vinput -n "__fish_seen_subcommand_from trigger trig; and not __fish_seen_subcommand_from list ls start stop" -a "list ls" -d "List configured triggers"
complete -c vinput -n "__fish_seen_subcommand_from trigger trig; and not __fish_seen_subcommand_from list ls start stop" -a start -d "Start trigger"
complete -c vinput -n "__fish_seen_subcommand_from trigger trig; and not __fish_seen_subcommand_from list ls start stop" -a stop -d "Stop trigger"
complete -c vinput -n "__fish_seen_subcommand_from trigger trig; and __fish_seen_subcommand_from start stop" -a "(__fish_vinput_installed_triggers)"

# receiver
complete -c vinput -n "__fish_seen_subcommand_from receiver recv; and not __fish_seen_subcommand_from list ls start stop" -a "list ls" -d "List configured receivers"
complete -c vinput -n "__fish_seen_subcommand_from receiver recv; and not __fish_seen_subcommand_from list ls start stop" -a start -d "Start receiver"
complete -c vinput -n "__fish_seen_subcommand_from receiver recv; and not __fish_seen_subcommand_from list ls start stop" -a stop -d "Stop receiver"
complete -c vinput -n "__fish_seen_subcommand_from receiver recv; and __fish_seen_subcommand_from start stop" -a "(__fish_vinput_installed_receivers)"

# recording
complete -c vinput -n "__fish_seen_subcommand_from recording rec; and not __fish_seen_subcommand_from start stop toggle" -a start -d "Start recording"
complete -c vinput -n "__fish_seen_subcommand_from recording rec; and not __fish_seen_subcommand_from start stop toggle" -a stop -d "Stop recording and recognize"
complete -c vinput -n "__fish_seen_subcommand_from recording rec; and not __fish_seen_subcommand_from start stop toggle" -a toggle -d "Toggle recording"
complete -c vinput -n "__fish_seen_subcommand_from recording rec; and __fish_seen_subcommand_from stop toggle" -s s -l scene -a "(__fish_vinput_installed_scenes)" -d "Scene ID"
