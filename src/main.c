#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>
#include <readline/readline.h>
#include <readline/history.h>

// Array for commands autocompletion
const char *builtins[] = {"exit", "echo", "type", "pwd", "cd", NULL};

static void free_string_list(char **list, size_t count) {
  if (list == NULL) return;
  for (size_t i = 0; i < count; i++) free(list[i]);
  free(list);
}

static int string_list_contains(char **list, size_t count, const char *s) {
  for (size_t i = 0; i < count; i++) {
    if (strcmp(list[i], s) == 0) return 1;
  }
  return 0;
}

static void string_list_push_unique(char ***list, size_t *count, size_t *cap, const char *s) {
  if (string_list_contains(*list, *count, s)) return;

  if (*count + 1 > *cap) {
    size_t new_cap = (*cap == 0) ? 16 : (*cap * 2);
    char **new_list = realloc(*list, new_cap * sizeof(char *));
    if (new_list == NULL) return;
    *list = new_list;
    *cap = new_cap;
  }

  char *copy = malloc(strlen(s) + 1);
  if (copy == NULL) return;
  strcpy(copy, s);
  (*list)[(*count)++] = copy;
}

// Autocompletion
char *command_generator(const char *text, int state) {
  static char **candidates = NULL;
  static size_t candidate_count = 0;
  static size_t candidate_cap = 0;
  static size_t candidate_index = 0;
  static int len = 0;
  
  if (!state) {
    len = strlen(text);
    candidate_index = 0;
    free_string_list(candidates, candidate_count);
    candidates = NULL;
    candidate_count = 0;
    candidate_cap = 0;

    for (int i = 0; builtins[i] != NULL; i++) {
      if (strncmp(builtins[i], text, len) == 0) {
        string_list_push_unique(&candidates, &candidate_count, &candidate_cap, builtins[i]);
      }
    }

    const char *path_env = getenv("PATH");
    if (path_env != NULL && path_env[0] != '\0') {
      char *path_copy = malloc(strlen(path_env) + 1);
      if (path_copy != NULL) {
        strcpy(path_copy, path_env);

        const char *delims = strchr(path_copy, ';') ? ";:" : ":";
        char *saveptr = NULL;
        for (char *dir = strtok_r(path_copy, delims, &saveptr); dir != NULL;
             dir = strtok_r(NULL, delims, &saveptr)) {
          if (dir[0] == '\0') continue;

          DIR *dp = opendir(dir);
          if (dp == NULL) {
            continue; 
          }

          struct dirent *entry;
          while ((entry = readdir(dp)) != NULL) {
            const char *name = entry->d_name;
            if (name[0] == '.') continue;
            if (strncmp(name, text, len) != 0) continue;

            char full_path[1024];
            size_t dlen = strlen(dir);
            int needs_slash = (dlen > 0 && dir[dlen - 1] != '/');
            if (snprintf(full_path, sizeof(full_path), "%s%s%s", dir, needs_slash ? "/" : "", name) < 0) {
              continue;
            }
            if (access(full_path, X_OK) == 0) {
              string_list_push_unique(&candidates, &candidate_count, &candidate_cap, name);
            }
          }

          closedir(dp);
        }

        free(path_copy);
      }
    }
  }
  
  if (candidate_index >= candidate_count) return NULL;

  char *match = malloc(strlen(candidates[candidate_index]) + 1);
  if (match == NULL) return NULL;
  strcpy(match, candidates[candidate_index]);
  candidate_index++;
  return match;
}

char **command_completion(const char *text, int start, int end) {
  if (start == 0) {
    rl_completion_append_character = ' ';
    rl_attempted_completion_over = 1;

    char **matches = rl_completion_matches(text, command_generator);
    if (matches == NULL) {
      putchar('\x07');
      fflush(stdout);
    }
    return matches;
  }
  return NULL;
}

int main(int argc, char *argv[]) {
  // Flush after every printf
  setbuf(stdout, NULL);
  
  rl_attempted_completion_function = command_completion;
  
  while (1) {
    char *input = readline("$ ");
    
    // Handle EOF (Ctrl+D)
    if (input == NULL) {
      break;
    }
    
    // Skip empty input
    if (strlen(input) == 0) {
      free(input);
      continue;
    }
    
    // Add to history
    add_history(input);
    
    if (strcmp(input, "exit") == 0) {
      free(input);
      break;
    } else if (strcmp(input, "pwd") == 0) {
      // pwd cmd
      char cwd[1024];
      if (getcwd(cwd, sizeof(cwd)) != NULL) {
        printf("%s\n", cwd);
      } else {
        perror("getcwd failed");
      }
    } else if (strncmp(input, "cd ", 3) == 0) {
      // cd cmd
      char *directory = input + 3;
      // Remove trailing whitespace if present
      int len = strlen(directory);
      while (len > 0 && (directory[len-1] == ' ' || directory[len-1] == '\t' || directory[len-1] == '\n')) {
        directory[len-1] = '\0';
        len--;
      }
      
      char expanded_path[1024];
      if (directory[0] == '~') {
        const char *home = getenv("HOME");
        if (home == NULL) {
          printf("cd: %s: No such file or directory\n", directory);
        } else {
          // Replace ~ with home directory
          if (directory[1] == '\0') {
            strcpy(expanded_path, home);
          } else if (directory[1] == '/') {
            snprintf(expanded_path, sizeof(expanded_path), "%s%s", home, directory + 1);
          } else {
            printf("cd: %s: No such file or directory\n", directory);
            free(input);
            continue;
          }
          directory = expanded_path;
          
          if (chdir(directory) != 0) {
            printf("cd: %s: No such file or directory\n", input + 3);
          }
        }
      } else if (chdir(directory) != 0) {
        printf("cd: %s: No such file or directory\n", input + 3);
      }
    } else if (strncmp(input, "echo ", 5) == 0) {
      printf("%s\n", input + 5);
    } else if (strncmp(input, "type ", 5) == 0) {
      // Extract the argument after "type " command
      char *arg = input + 5;
      // Check if the argument is a shell builtin
      if (!strcmp(arg, "exit") || !strcmp(arg, "echo") || !strcmp(arg, "type") || !strcmp(arg, "pwd") || !strcmp(arg, "cd")) {
        printf("%s is a shell builtin\n", arg);
      } else {
        // Search for the command in PATH
        char *path_env = getenv("PATH");
        if (path_env == NULL) {
          printf("%s: not found\n", arg);
          free(input);
          continue;
        }
        
        char *path_copy = malloc(strlen(path_env) + 1);
        strcpy(path_copy, path_env);
        
        int found = 0;
        char *directory = strtok(path_copy, ":");
        
        while (directory != NULL && !found) {
          char full_path[512];
          snprintf(full_path, sizeof(full_path), "%s/%s", directory, arg);
          
          if (access(full_path, X_OK) == 0) {
            printf("%s is %s\n", arg, full_path);
            found = 1;
          }
          
          directory = strtok(NULL, ":");
        }
        
        free(path_copy);
        
        if (!found) {
          printf("%s: not found\n", arg);
        }
      }
    } else {
      char input_copy[100];
      strcpy(input_copy, input);
      
      int arg_count = 0;
      char *temp = input_copy;
      while (*temp) {
        if (*temp != ' ' && (temp == input_copy || *(temp - 1) == ' ')) {
          arg_count++;
        }
        temp++;
      }
      
      char **args = malloc((arg_count + 1) * sizeof(char*));
      
      strcpy(input_copy, input);
      args[0] = strtok(input_copy, " ");
      for (int i = 1; i < arg_count; i++) {
        args[i] = strtok(NULL, " ");
      }
      args[arg_count] = NULL;
      
      char *path_env = getenv("PATH");
      if (path_env == NULL) {
        printf("%s: command not found\n", args[0]);
        free(args);
        free(input);
        continue;
      }
      
      char *path_copy = malloc(strlen(path_env) + 1);
      strcpy(path_copy, path_env);
      
      int found = 0;
      char full_path[512];
      char *directory = strtok(path_copy, ":");
      
      while (directory != NULL && !found) {
        snprintf(full_path, sizeof(full_path), "%s/%s", directory, args[0]);
        
        if (access(full_path, X_OK) == 0) {
          found = 1;
        } else {
          directory = strtok(NULL, ":");
        }
      }
      
      free(path_copy);
      
      if (found) {
        pid_t pid = fork();
        if (pid == 0) {
          execv(full_path, args);
          perror("execv failed");
          exit(1);
        } else if (pid > 0) {
          wait(NULL);
        } else {
          perror("fork failed");
        }
      } else {
        printf("%s: command not found\n", args[0]);
      }
      
      free(args);
    }
    
    free(input);
  }
  return 0;
}
