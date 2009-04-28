#ifndef vte_controller_h_included
#define vte_controller_h_included

#include <glib.h>

#include "vte.h"

typedef struct InputNode {
  gchar charData;
  struct InputNode *previous;
  struct InputNode *next;
} InputNode;

typedef struct VteCommandHistoryNode {
	gchar *data;
	struct VteCommandHistoryNode *previous;
	struct VteCommandHistoryNode *next;
} VteCommandHistoryNode;

typedef struct ConsoleController {
	VteTerminal *terminal;

	/* Pending user input */
	InputNode *input_cursor;
	InputNode *input_head;
	glong input_length;
	glong input_cursor_position;

	/* Command history */
	VteCommandHistoryNode *cmd_history;
	VteCommandHistoryNode *last_cmd;

	/* Command prompt */
	gchar *prompt;
	glong prompt_length;

	/* Is the data being fed by the user or by the app? */
	gboolean user_input_mode;
} ConsoleController;

ConsoleController *console_controller_new(VteTerminal *terminal);
void console_controller_free(ConsoleController *controller);

void console_controller_start_user_input(ConsoleController *controller);
void console_controller_stop_user_input(ConsoleController *controller);

void console_controller_set_command_prompt(ConsoleController *controller, const gchar *text);
void console_controller_print_command_prompt(ConsoleController *controller);

void console_controller_user_input(ConsoleController *controller, gchar *text);

/* Mess with the command history buffer */
void console_controller_command_history_back(ConsoleController *controller);
void console_controller_command_history_forward(ConsoleController *controller);

/* Flush any pending user input to listeners and reset */
void console_controller_flush_pending_input(ConsoleController *controller);

/* Captures cursor movement inside pending input */
void console_controller_cursor_left(ConsoleController *controller);
void console_controller_cursor_right(ConsoleController *controller);

/* Removes a single character from pending input */
void console_controller_delete_current_char(ConsoleController *controller);

void console_controller_cursor_home(ConsoleController *controller);
void console_controller_cursor_end(ConsoleController *controller);

gboolean console_controller_check_cursor_at_beginning(ConsoleController *ctrl);

#endif
