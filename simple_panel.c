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

#include <curses.h>
#include <stdlib.h>
#include "simple_panel.h"

static PANEL *panels;

void update_panels (void)
{
	PANEL *p = panels;
	while (p) {
		touchwin (p->wnd);
		if (p->is_pad)
			pnoutrefresh (p->wnd, p->pminrow, p->pmincol,
				      p->sminrow, p->smincol, p->smaxrow - 1,
				      p->smaxcol - 1);
		else
			wnoutrefresh (p->wnd);
		
		p = p->above;
	}
}

PANEL *new_panel (WINDOW *wnd)
{
	PANEL **pp, *newp = malloc (sizeof (PANEL));
	if (!newp)
		return NULL;

	newp->above = NULL;
	newp->wnd = wnd;
	newp->is_pad = 0;
	pp = &panels;
	while (*pp)
		pp = &(*pp)->above;
	*pp = newp;
	return newp;
}

void panel_is_pad (PANEL *pan, int pminrow, int pmincol,
		   int sminrow, int smincol, int smaxrow, int smaxcol)
{
	pan->is_pad = 1;
	pan->pminrow = pminrow;
	pan->pmincol = pmincol;
	pan->sminrow = sminrow;
	pan->smincol = smincol;
	pan->smaxrow = smaxrow;
	pan->smaxcol = smaxcol;
}

static int do_del_panel (PANEL *pan)
{
	PANEL *p;
	if (panels == pan) {
		panels = panels->above;
		return 0;
	}
	p = panels;
	while (p->above) {
		if (p->above == pan) {
			p->above = pan->above;
			return 0;
		}
		p = p->above;
	}

	return 1;
}

int del_panel (PANEL *pan)
{
	int ret = do_del_panel (pan);
	free (pan);
	return ret;
}

WINDOW *panel_window (PANEL *pan)
{
	return pan->wnd;
}

int bottom_panel (PANEL *pan)
{
	do_del_panel (pan);
	pan->above = panels;
	panels = pan;
	return 0;
}
