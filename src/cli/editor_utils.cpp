#include "cli/editor_utils.h"

#include <cstdlib>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

int OpenInEditor(const std::filesystem::path& file_path) {
  const char* editor = getenv("VISUAL");
  if (!editor || editor[0] == '\0') {
    editor = getenv("EDITOR");
  }
  if (!editor || editor[0] == '\0') {
    editor = "vi";
  }

  pid_t pid = fork();
  if (pid < 0) {
    return -1;
  }
  if (pid == 0) {
    // Child
    std::string path_str = file_path.string();
    char* args[] = {const_cast<char*>(editor),
                    const_cast<char*>(path_str.c_str()),
                    nullptr};
    execvp(editor, args);
    // execvp failed
    _exit(127);
  }

  // Parent: wait for editor to exit
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    return -1;
  }
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  return -1;
}
