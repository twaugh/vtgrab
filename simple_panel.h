/*
 * vtgrab - grab the foreground console for display on another machine
 * Copyright (C) 2000  Tim Waugh <twaugh@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *
 * This file contains a simple reimplementation of ncurses's panel,
 * with the advatnage of being able to interoperate with pads.  It
 * doesn't, oever, cope well with overlapping panels.
 */

typedef struct panel {
	WINDOW *wnd;
	int is_pad;
	int pminrow;
	int pmincol;
	int sminrow;
	int smincol;
	int smaxrow;
	int smaxcol;
	struct panel *above;
} PANEL;

extern void update_panels (void);
extern PANEL *new_panel (WINDOW *);
extern void panel_is_pad (PANEL *, int pminrow, int pmincol,
			  int sminrow, int smincol, int smaxrow, int smaxcol);
extern int del_panel (PANEL *);
extern WINDOW *panel_window (PANEL *);
extern int bottom_panel (PANEL *);
