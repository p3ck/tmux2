/* $Id$ */

/*
 * Copyright (c) 2008 Nicholas Marriott <nicm@users.sourceforge.net>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <stdlib.h>
#include <string.h>

#include "tmux.h"

#define DUMP_STATE_HISTORY_UTF8_BUFFER_SIZE ((UTF8_SIZE) * 2 + 1)
#define DUMP_STATE_HISTORY_CONTEXT_SIZE 4

/*
 * Output information needed by control clients, including history, cursor
 * position, and miscellaneous VT100 state.
 */

int cmd_dump_state_exec(struct cmd *, struct cmd_ctx *);

const struct cmd_entry cmd_dump_state_entry = {
	"dump-state", "dumpstate",
	"hael:t:k:", 0, 0,
	"[-hae] [-l lines] [-t target-pane] [-k key]",
	0,
	NULL,
	NULL,
	cmd_dump_state_exec
};

static void
dump_state_uint(struct cmd_ctx *ctx, unsigned int value, const char *name)
{
	ctx->print(ctx, "%s=%u", name, value);
}

static void
dump_state_int(struct cmd_ctx *ctx, unsigned int value, const char *name)
{
	ctx->print(ctx, "%s=%d", name, value);
}

static void
dump_state_bits(struct cmd_ctx *ctx, bitstr_t *value, int length, const char *name)
{
	struct dstring	ds;
	int		separator = 0;

	ds_init(&ds);
	for (int i = 0; i < length; i++) {
		if (bit_test(value, i)) {
			if (separator) {
				ds_append(&ds, ",");
			} else {
				separator = 1;
			}
			ds_appendf(&ds, "%d", i);
		}
	}
	ctx->print(ctx, "%s=%s", name, ds.buffer);
	ds_free(&ds);
}

static void
dump_state_string(struct cmd_ctx *ctx, char *str, const char *name)
{
	struct dstring	ds;
	ds_init(&ds);

	ctx->print(ctx, "%s=%s", name, str);
	ds_free(&ds);
}

/* Return a hex encoded version of utf8data. */
static char *
dump_state_history_encode_utf8(struct grid_utf8 *utf8data, char *buffer)
{
	int		o;
	unsigned int	i;
	unsigned char	c;

	o = 0;
	size_t size = grid_utf8_size(utf8data);
	for (i = 0; i < size; i++) {
		c = utf8data->data[i];
		sprintf(buffer + o, "%02x", (int)c);
		o += 2;
	}
	return buffer;
}

static void
dump_state_history_output_last_char(struct dstring *last_char, struct dstring *output,
				    int *repeats)
{
	if (last_char->used > 0) {
		ds_append(output, last_char->buffer);
		if (*repeats == 2 && last_char->used <= 3) {
			/* If an ASCII code repeats once then it's shorter to print it
			 * twice than to use the run-length encoding. */
			ds_append(output, last_char->buffer);
		} else if (*repeats > 1) {
			/* Output "*<n> " to indicate that the last character repeats
			 * <n> times. For instance, "AAA" is represented as "61*3". */
			ds_appendf(output, "*%d ", *repeats);
		}
		ds_truncate(last_char, 0);
	}
}

static void
dump_state_history_append_char(struct grid_cell *celldata, struct grid_utf8 *utf8data,
			       struct dstring *last_char, int *repeats,
			       struct dstring *output)
{
	struct dstring	ds;
	ds_init(&ds);

	if (celldata->flags & GRID_FLAG_UTF8) {
		char temp[DUMP_STATE_HISTORY_UTF8_BUFFER_SIZE + 3];
		dump_state_history_encode_utf8(utf8data, temp);
		ds_appendf(&ds, "[%s]", dump_state_history_encode_utf8(utf8data, temp));
	} else {
		ds_appendf(&ds, "%x", ((int) celldata->data) & 0xff);
	}
	if (last_char->used > 0 && !strcmp(ds.buffer, last_char->buffer)) {
		/* Last character repeated */
		(*repeats)++;
	} else {
		/* Not a repeat */
		dump_state_history_output_last_char(last_char, output, repeats);
		ds_append(last_char, ds.buffer);
		*repeats = 1;
	}
}

static void
dump_state_history_cell(struct dstring *output, struct grid_cell *celldata,
			struct grid_utf8 *utf8data, int *dump_context,
			struct dstring *last_char, int *repeats)
{
	int	flags;

	/* Exclude the GRID_FLAG_UTF8 flag because it's wasteful to output when
	 * UTF-8 chars are already marked by being enclosed in square brackets. */
	flags  = celldata->flags & (GRID_FLAG_FG256 | GRID_FLAG_BG256 | GRID_FLAG_PADDING);
	if (celldata->attr != dump_context[0] ||
	    flags != dump_context[1] ||
	    celldata->fg != dump_context[2] ||
	    celldata->bg != dump_context[3]) {
		/* Context has changed since the last character. */
		dump_context[0] = celldata->attr;
		dump_context[1] = flags;
		dump_context[2] = celldata->fg;
		dump_context[3] = celldata->bg;

		dump_state_history_output_last_char(last_char, output, repeats);
		ds_appendf(output, ":%x,%x,%x,%x,", celldata->attr,
			   celldata->flags, celldata->fg, celldata->bg);
	}
	dump_state_history_append_char(celldata, utf8data, last_char, repeats, output);
}

static void
dump_state_history_line(struct cmd_ctx *ctx, struct grid_line *linedata,
				  int *dump_context)
{
	unsigned int	i;
	struct dstring	last_char;
	struct dstring	output;

	ds_init(&output);
	ds_init(&last_char);
	int repeats = 0;
	for (i = 0; i < linedata->cellsize; i++) {
		dump_state_history_cell(&output, linedata->celldata + i,
					linedata->utf8data + i, dump_context, &last_char,
					&repeats);
	}
	dump_state_history_output_last_char(&last_char, &output, &repeats);
	if (linedata->flags & GRID_LINE_WRAPPED) {
		ds_appendf(&output, "+");
	}
	ctx->print(ctx, "%s", output.buffer);
	ds_free(&output);
}

static int
dump_state_history(struct cmd *self, struct cmd_ctx *ctx)
{
	struct args		*args = self->args;
	struct window_pane	*wp;
	struct session		*s;
	const char		*max_lines_str;
	unsigned		 max_lines;
	int			 temp;
	unsigned int		 i;
	unsigned int		 start, limit;
	struct grid		*grid;
	int 			 dump_context[DUMP_STATE_HISTORY_CONTEXT_SIZE] =
	    { -1, -1, -1, -1 };

	if (cmd_find_pane(ctx, args_get(args, 't'), &s, &wp) == NULL)
		return (-1);

	max_lines_str = args_get(args, 'l');
	if (!max_lines_str)
		return (-1);
	temp = atoi(max_lines_str);
	if (temp <= 0)
		return (-1);
	max_lines = temp;  /* assign to unsigned to do comparisons later */

	if (args_has(args, 'a')) {
		grid = wp->saved_grid;
		if (!grid)
			return (0);
	} else
		grid = wp->base.grid;
	limit = grid->hsize + grid->sy;
	if (limit >= max_lines)
		start = limit - max_lines;
	else
		start = 0;
	for (i = start; i < limit; i++)
		dump_state_history_line(ctx, grid->linedata + i, dump_context);
	return (0);
}

static int
dump_state_emulator(struct cmd *self, struct cmd_ctx *ctx)
{
	struct args		*args = self->args;
	struct window_pane	*wp;
	struct session		*s;

	if (cmd_find_pane(ctx, args_get(args, 't'), &s, &wp) == NULL)
		return (-1);

	dump_state_int(ctx, wp->saved_grid ? 1 : 0, "in_alternate_screen");
	/* This is the saved cursor position from when the alternate screen was
	 * entered. */
	dump_state_uint(ctx, wp->saved_cx, "base_cursor_x");
	dump_state_uint(ctx, wp->saved_cy, "base_cursor_y");
	dump_state_uint(ctx, wp->base.cx, "cursor_x");
	dump_state_uint(ctx, wp->base.cy, "cursor_y");
	dump_state_uint(ctx, wp->base.rupper, "scroll_region_upper");
	dump_state_uint(ctx, wp->base.rlower, "scroll_region_lower");
	dump_state_bits(ctx, wp->base.tabs, wp->base.grid->sx, "tabstops");
	dump_state_string(ctx, wp->base.title, "title");

	/* This is the saved cursor position from CSI DECSC. */
	dump_state_int(ctx, wp->ictx.old_cx, "decsc_cursor_x");
	dump_state_int(ctx, wp->ictx.old_cy, "decsc_cursor_y");

	return (0);
}

static int
dump_state_kvp(struct cmd *self, struct cmd_ctx *ctx)
{
	struct args	*args = self->args;
	char		*name;
	char		*value;

	name = args_get(args, 'k');
	if (!name)
		return (-1);

	value = control_get_kvp_value(name);
	if (value)
		ctx->print(ctx, "%s", value);
	else
		ctx->print(ctx, "", value);

	return (0);
}

int
cmd_dump_state_exec(struct cmd *self, struct cmd_ctx *ctx)
{
	struct args	*args = self->args;

	if (args_has(args, 'e'))
		return dump_state_emulator(self, ctx);
	else if (args_has(args, 'h'))
		return dump_state_history(self, ctx);
	else if (args_has(args, 'k'))
		return dump_state_kvp(self, ctx);
	else
		return (-1);
}
