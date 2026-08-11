#include "menu.h"

#include "esp_bringup.h"
#include "output.h"

#include "sdkconfig.h"

#define MAX_MENU_DEPTH 4
#define PROMPT_BUF_SIZE 64

/* Path from the root to the menu the user has drilled into. */
static const bp_menu_t *menu_stack[MAX_MENU_DEPTH];
static size_t menu_depth;
static char prompt_buf[PROMPT_BUF_SIZE];

static const char *const exit_words[] = {"back", "exit", "quit"};

static const bp_menu_t *current_menu(void)
{
    return menu_depth ? menu_stack[menu_depth - 1] : &bp_root_menu;
}

static bool is_exit_word(const char *token)
{
    for (size_t i = 0; i < sizeof(exit_words) / sizeof(exit_words[0]); i++) {
        if (strcasecmp(token, exit_words[i]) == 0) {
            return true;
        }
    }
    return false;
}

static const bp_menu_t *find_submenu(const bp_menu_t *menu, const char *name)
{
    for (size_t i = 0; i < menu->submenu_count; i++) {
        if (strcasecmp(menu->submenus[i]->name, name) == 0) {
            return menu->submenus[i];
        }
    }
    return NULL;
}

static const bp_command_t *find_command(const bp_menu_t *menu, const char *name)
{
    for (size_t i = 0; i < menu->command_count; i++) {
        if (strcasecmp(menu->commands[i].name, name) == 0) {
            return &menu->commands[i];
        }
    }
    return NULL;
}

static void rebuild_prompt(void)
{
    int used = snprintf(prompt_buf, sizeof(prompt_buf), "%s", CONFIG_IDF_TARGET);

    for (size_t i = 0; i < menu_depth; i++) {
        int n = snprintf(prompt_buf + used, sizeof(prompt_buf) - (size_t)used,
                         "/%s", menu_stack[i]->name);
        if (n < 0 || (size_t)(used + n) >= sizeof(prompt_buf)) {
            break;
        }
        used += n;
    }

    snprintf(prompt_buf + used, sizeof(prompt_buf) - (size_t)used, "> ");
}

const char *bp_menu_prompt(void)
{
    if (prompt_buf[0] == '\0') {
        rebuild_prompt();
    }
    return prompt_buf;
}

/* Full path to a menu, for help text: "gpio pwm". */
static void menu_path(const bp_menu_t *menu, const bp_menu_t *root,
                      char *out, size_t out_size)
{
    out[0] = '\0';
    if (menu == root) {
        return;
    }

    /* Depth is small, so a search from the root is cheaper than back-pointers. */
    for (size_t i = 0; i < root->submenu_count; i++) {
        const bp_menu_t *child = root->submenus[i];
        if (child == menu) {
            snprintf(out, out_size, "%s", child->name);
            return;
        }
        char nested[PROMPT_BUF_SIZE];
        menu_path(menu, child, nested, sizeof(nested));
        if (nested[0] != '\0') {
            snprintf(out, out_size, "%s %s", child->name, nested);
            return;
        }
    }
}

void bp_menu_print_help(const bp_menu_t *menu)
{
    char path[PROMPT_BUF_SIZE];
    menu_path(menu, &bp_root_menu, path, sizeof(path));
    const char *prefix = path[0] ? path : "";
    const char *space = path[0] ? " " : "";

    if (menu->help) {
        bp_printf("\n%s\n", menu->help);
    }

    if (menu->command_count) {
        bp_printf("\nCommands:\n");
        for (size_t i = 0; i < menu->command_count; i++) {
            const bp_command_t *cmd = &menu->commands[i];
            char syntax[80];
            snprintf(syntax, sizeof(syntax), "%s%s%s%s%s", prefix, space, cmd->name,
                     cmd->usage && cmd->usage[0] ? " " : "", cmd->usage ? cmd->usage : "");
            bp_printf("  %-34s %s\n", syntax, cmd->help ? cmd->help : "");
        }
    }

    if (menu->submenu_count) {
        bp_printf("\nMenus:\n");
        for (size_t i = 0; i < menu->submenu_count; i++) {
            const bp_menu_t *sub = menu->submenus[i];
            char syntax[80];
            snprintf(syntax, sizeof(syntax), "%s%s%s", prefix, space, sub->name);
            bp_printf("  %-34s %s\n", syntax, sub->help ? sub->help : "");
        }
    }

    if (menu != &bp_root_menu) {
        bp_printf("\nUse 'back', 'exit' or 'quit' to leave this menu.\n");
    }
    bp_printf("\n");
}

/*
 * Walk `menu` consuming tokens. On return:
 *   *out_cmd  set  -> a command was reached; *index is its token position
 *   *out_menu set  -> tokens ran out inside a menu; the caller should drill in
 * Returns 0 if anything was resolved, -1 otherwise.
 */
static int resolve(const bp_menu_t *menu, int argc, char **argv, int index,
                   const bp_command_t **out_cmd, const bp_menu_t **out_menu,
                   int *out_index)
{
    *out_cmd = NULL;
    *out_menu = NULL;

    while (index < argc) {
        const bp_command_t *cmd = find_command(menu, argv[index]);
        if (cmd) {
            *out_cmd = cmd;
            *out_index = index;
            return 0;
        }

        const bp_menu_t *sub = find_submenu(menu, argv[index]);
        if (!sub) {
            return -1;
        }

        menu = sub;
        index++;
    }

    /* Tokens exhausted on a menu name: the user wants to drill in. */
    *out_menu = menu;
    return 0;
}

/*
 * Record the path from `from` down to `target` into menu_stack, starting at
 * stack slot `depth`. Returns the resulting depth, or 0 if `target` is not
 * reachable from `from`.
 */
static size_t find_path(const bp_menu_t *from, const bp_menu_t *target, size_t depth)
{
    if (depth >= MAX_MENU_DEPTH) {
        return 0;
    }

    for (size_t i = 0; i < from->submenu_count; i++) {
        const bp_menu_t *child = from->submenus[i];
        menu_stack[depth] = child;

        if (child == target) {
            return depth + 1;
        }

        size_t found = find_path(child, target, depth + 1);
        if (found) {
            return found;
        }
    }

    return 0;
}

static void push_menu(const bp_menu_t *menu)
{
    /* Rebuild the whole path, so drilling in with a fully qualified name from
     * an unrelated menu ("gpio pwm" while inside i2c) lands in the right place. */
    menu_depth = (menu == &bp_root_menu) ? 0 : find_path(&bp_root_menu, menu, 0);
}

int bp_menu_execute(int argc, char **argv)
{
    if (argc == 0) {
        return 0;
    }

    if (is_exit_word(argv[0])) {
        if (menu_depth == 0) {
            bp_printf("Already at the top level.\n");
        } else {
            menu_depth--;
        }
        rebuild_prompt();
        return 0;
    }

    if (strcasecmp(argv[0], "help") == 0 || strcmp(argv[0], "?") == 0) {
        const bp_menu_t *target = current_menu();
        if (argc > 1) {
            const bp_command_t *cmd = NULL;
            const bp_menu_t *menu = NULL;
            int index = 0;
            /* "help gpio pwm" -> help for the pwm menu. */
            if (resolve(current_menu(), argc, argv, 1, &cmd, &menu, &index) == 0 && menu) {
                target = menu;
            } else if (resolve(&bp_root_menu, argc, argv, 1, &cmd, &menu, &index) == 0 && menu) {
                target = menu;
            } else {
                bp_error("No such menu: %s", argv[1]);
                return -1;
            }
        }
        bp_menu_print_help(target);
        return 0;
    }

    const bp_command_t *cmd = NULL;
    const bp_menu_t *menu = NULL;
    int index = 0;

    /* Try the current menu first, then the root, so a fully qualified command
     * ("gpio set 19 true") works from inside any other menu. */
    int rc = resolve(current_menu(), argc, argv, 0, &cmd, &menu, &index);
    if (rc < 0 && current_menu() != &bp_root_menu) {
        rc = resolve(&bp_root_menu, argc, argv, 0, &cmd, &menu, &index);
    }

    if (rc < 0) {
        bp_error("Unknown command: %s", argv[0]);
        bp_printf("Type 'help' for available commands.\n");
        return -1;
    }

    if (menu) {
        push_menu(menu);
        rebuild_prompt();
        bp_menu_print_help(menu);
        return 0;
    }

    return cmd->fn(argc - index, &argv[index]);
}

size_t bp_menu_complete(const char *prefix, const char **out, size_t out_max)
{
    size_t count = 0;
    size_t prefix_len = strlen(prefix);

    const bp_menu_t *menus[2] = {current_menu(), &bp_root_menu};
    size_t menu_count = (menus[0] == menus[1]) ? 1 : 2;

    for (size_t m = 0; m < menu_count && count < out_max; m++) {
        const bp_menu_t *menu = menus[m];

        for (size_t i = 0; i < menu->command_count && count < out_max; i++) {
            if (strncasecmp(menu->commands[i].name, prefix, prefix_len) == 0) {
                out[count++] = menu->commands[i].name;
            }
        }
        for (size_t i = 0; i < menu->submenu_count && count < out_max; i++) {
            if (strncasecmp(menu->submenus[i]->name, prefix, prefix_len) == 0) {
                out[count++] = menu->submenus[i]->name;
            }
        }
    }

    return count;
}
