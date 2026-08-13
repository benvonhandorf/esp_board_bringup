#ifndef BOARD_H
#define BOARD_H

/*
 * Named board pinouts.
 *
 * Every command in this project takes explicit pin numbers, which is right for
 * a tool whose job is to find out how a board is wired. Once a board *has*
 * been worked out, though, retyping its pinout is only a way to make mistakes.
 * A board entry records the answer, and a preset replays the commands that use
 * it.
 *
 * Each board is a submenu and each subsystem it supports is a command inside
 * it, so `board cardputer` prints the pinout menu and changes nothing, while
 * `board cardputer audio` runs the setup and echoes every line it runs.
 * Adding a board is an entry in board.c and a submenu in menu_table.c.
 */

int cmd_board_list(int argc, char **argv);

int cmd_board_cardputer_pins(int argc, char **argv);
int cmd_board_cardputer_audio(int argc, char **argv);
int cmd_board_cardputer_sd(int argc, char **argv);

int cmd_board_xiao_pins(int argc, char **argv);
int cmd_board_xiao_sd(int argc, char **argv);

#endif
