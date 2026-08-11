#ifndef MENU_H
#define MENU_H

#include <stddef.h>

/*
 * Hierarchical command registry.
 *
 * The README describes menus you can either drill into ("gpio") or address in
 * one shot ("gpio set 19 true"), nested one level deeper for PWM
 * ("gpio pwm set ..."). Menus therefore nest arbitrarily, and dispatch walks
 * the tree consuming tokens until it lands on a command.
 *
 * A command receives argv[0] == its own name, so "gpio set 19 true" reaches
 * cmd_gpio_set() as argc=3, argv={"set", "19", "true"}.
 */

typedef int (*bp_command_fn)(int argc, char **argv);

typedef struct {
    const char *name;
    const char *usage; /* argument syntax, without the menu path */
    const char *help;  /* one-line description */
    bp_command_fn fn;
} bp_command_t;

typedef struct bp_menu {
    const char *name;
    const char *help;
    const bp_command_t *commands;
    size_t command_count;
    const struct bp_menu *const *submenus;
    size_t submenu_count;
} bp_menu_t;

/* The root of the tree; defined in menu_table.c. */
extern const bp_menu_t bp_root_menu;

/*
 * Execute one command line. Tokens are matched case-insensitively (README:
 * "All commands are case insensitive"); argument *values* are never case
 * folded, so passwords and UART payloads survive intact.
 *
 * Returns the command's own return value, or -1 if the line could not be
 * resolved. Navigation lines ("gpio", "back") return 0.
 */
int bp_menu_execute(int argc, char **argv);

/* Current prompt, e.g. "esp32c3/gpio/pwm> ". Valid until the next dispatch. */
const char *bp_menu_prompt(void);

/* True if the line was a menu navigation command rather than a real command. */
void bp_menu_print_help(const bp_menu_t *menu);

/* Tab-completion support: fill `out` with candidate tokens for `prefix`. */
size_t bp_menu_complete(const char *prefix, const char **out, size_t out_max);

#endif
