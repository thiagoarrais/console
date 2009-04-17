#ifndef vte_controller_h_included
#define vte_controller_h_included

#include "vte.h"

void console_controller_start_user_input(VteTerminal *terminal);
void console_controller_stop_user_input(VteTerminal *terminal);

void console_controller_user_input(VteTerminal *terminal, gchar *text);

/* Mess with the command history buffer */
void console_controller_command_history_back(VteTerminal *terminal);
void console_controller_command_history_forward(VteTerminal *terminal);

void console_controller_reset_pending_input(VteTerminal *terminal);

/* Flush any pending user input to listeners and reset */
void console_controller_flush_pending_input(VteTerminal *terminal);

/* Captures cursor movement inside pending input */
void console_controller_cursor_left(VteTerminal *terminal);
void console_controller_cursor_right(VteTerminal *terminal);

/* Removes a single character from pending input */
void console_controller_delete_current_char(VteTerminal *terminal);

void console_controller_cursor_home(VteTerminal *terminal);
void console_controller_cursor_end(VteTerminal *terminal);
void console_controller_clear_input(VteTerminal *terminal);


#endif
