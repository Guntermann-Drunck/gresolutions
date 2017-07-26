/*
 * gresolutions.c
 *
 * gresolutions is a GTK+ 3 based tool for quickly checking different video
 * modes using the RandR extension.
 *
 * Copyright (C) 2017 Dirk Eibach, Guntermann & Drunck GmbH <eibach@gdsys.de>.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gtk/gtk.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xrandr.h>

static XRRScreenResources *res;
static Display *dpy;
static Window root;
static int screen;

enum {
	XID_COLUMN,
	XID_STRING_COLUMN,
	NAME_COLUMN,
	REFRESH_COLUMN,
	PIXCLOCK_COLUMN,
	PREFERRED_COLUMN,
	N_COLUMNS
};

static unsigned char *output_edid_get(RROutput output, unsigned long *length)
{
	Atom edid = None, type = None;
	unsigned char *prop;
	int format = 0;
	unsigned long nitems = 0, bytes = 0;

	/* try to get the edid atom */
	if (!(edid = XInternAtom(dpy, RR_PROPERTY_RANDR_EDID, False)))
		return NULL;

	/* get the output property
	 * 
	 * NB: Returns 0 on success */
	if (!XRRGetOutputProperty
	    (dpy, output, edid, 0, 128, False, False, AnyPropertyType,
	     &type, &format, &nitems, &bytes, &prop)) {
		if ((type == XA_INTEGER) && (nitems >= 1) && (format == 8)) {
			unsigned char *ret = NULL;

			if ((ret = malloc(nitems * sizeof(unsigned char)))) {
				if (length)
					*length = nitems;
				memcpy(ret, prop,
				       (nitems * sizeof(unsigned char)));
				return ret;
			}
		}
	}

	return NULL;
}

static int parseedid(unsigned char *edid, unsigned char *modelname) {
	int i;
	int j;
	unsigned char sum = 0;

	//check the checksum
	for (i = 0; i<128; i++) {
		sum += edid[i];
	}

	if (sum)
		g_warning("edid checksum failed\n");

	//check header
	for (i = 0; i < 8; i++) {
		if (!(((i == 0 || i == 7) && edid[i] == 0x00) || (edid[i] == 0xff))) //0x00 0xff 0xff 0xff 0xff 0xff 0x00
			g_warning("edid header incorrect. Probably not an edid\n");
	}

	//Product Identification
	/* Model Name: Only thing I do out of order of edid, to comply with X standards... */
	for (i = 0x36; i < 0x7E; i += 0x12) { //read through descriptor blocks...
		if (edid[i] == 0x00) { //not a timing descriptor
			if (edid[i+3] == 0xfc) { //Model Name tag
				for (j = 0; j < 13; j++) {
					if (edid[i+5+j] == 0x0a)
						modelname[j] = 0x00;
					else
						modelname[j] = edid[i+5+j];
				}
			}
		}
	}

	return 0;
}

static XRRModeInfo *find_mode_by_xid(XRRScreenResources * res, RRMode xid)
{
	unsigned int k;

	for (k = 0; k < res->nmode; ++k) {
		XRRModeInfo *mode = &res->modes[k];

		if (xid == mode->id)
			return mode;
	}

	return NULL;
}

/* v refresh frequency in Hz */
static double mode_refresh(const XRRModeInfo * mode_info)
{
	double rate;
	double vTotal = mode_info->vTotal;

	if (mode_info->modeFlags & RR_DoubleScan) {
		/* doublescan doubles the number of lines */
		vTotal *= 2;
	}

	if (mode_info->modeFlags & RR_Interlace) {
		/* interlace splits the frame into two fields */
		/* the field rate is what is typically reported by monitors */
		vTotal /= 2;
	}

	if (mode_info->hTotal && vTotal)
		rate = ((double)mode_info->dotClock /
			((double)mode_info->hTotal * (double)vTotal));
	else
		rate = 0;
	return rate;
}

void row_activated(GtkTreeView * tree_view,
		   GtkTreePath * path,
		   GtkTreeViewColumn * column, gpointer user_data)
{
	GtkTreeModel *model;
	GtkTreeIter iter;

	model = gtk_tree_view_get_model(tree_view);
	if (gtk_tree_model_get_iter(model, &iter, path)) {
		int xid;
		XRROutputInfo *output_info =
		    XRRGetOutputInfo(dpy, res, *(XID *) user_data);

		gtk_tree_model_get(model, &iter, XID_COLUMN, &xid, -1);
		XRRSetCrtcConfig(dpy, res, output_info->crtc, CurrentTime, 0, 0,
				 xid, RR_Rotate_0, (XID *) user_data, 1);
	}
}

static void activate(GtkApplication * app, gpointer user_data)
{
	GtkWidget *window;
	GtkWidget *notebook;
	unsigned int k;
	char *label;

	dpy = XOpenDisplay(NULL);
	screen = DefaultScreen(dpy);
	root = RootWindow(dpy, screen);
	res = XRRGetScreenResources(dpy, root);

	window = gtk_application_window_new(app);
	asprintf(&label, "gresolutions%s", XDisplayString(dpy));
	gtk_window_set_title(GTK_WINDOW(window), label);
	free(label);
	gtk_window_set_default_size(GTK_WINDOW(window), 200, 200);

	notebook = gtk_notebook_new();
	gtk_container_add(GTK_CONTAINER(window), notebook);

	for (k = 0; k < res->noutput; k++) {
		unsigned int n;
		unsigned char *edid;
		unsigned long edid_length;
		char modelname[13] = "";
		char *label;

		XRROutputInfo *output_info =
		    XRRGetOutputInfo(dpy, res, res->outputs[k]);
		XRRCrtcInfo *crtc_info;
		GtkTreeIter iter;
		GtkWidget *tree;
		GtkTreeViewColumn *column;
		GtkCellRenderer *renderer;
		GtkListStore *list_store = gtk_list_store_new(N_COLUMNS,
							      G_TYPE_INT,
							      G_TYPE_STRING,
							      G_TYPE_STRING,
							      G_TYPE_STRING,
							      G_TYPE_STRING,
							      G_TYPE_BOOLEAN);

		if (output_info->connection == RR_Disconnected)
			continue;

		if (!output_info->crtc)
			continue;

		crtc_info = XRRGetCrtcInfo(dpy, res, output_info->crtc);
		if (!crtc_info)
			continue;

		edid = output_edid_get(res->outputs[k], &edid_length);
		if (edid && edid_length)
			parseedid(edid, modelname);

		for (n = 0; n < output_info->nmode; ++n) {
			char *xid_string;
			char *name;
			char *refresh;
			char *pixclock;
			XRRModeInfo *mode_info;

			mode_info =
			    find_mode_by_xid(res, output_info->modes[n]);
			if (!mode_info)
				continue;

			asprintf(&xid_string, "0x%x", output_info->modes[n]);
			asprintf(&name, mode_info->name);
			asprintf(&refresh, "%6.2fHz", mode_refresh(mode_info));
			asprintf(&pixclock, "%6.3fMHz",
				 (double)mode_info->dotClock / 1000000.0);

			gtk_list_store_append(list_store, &iter);
			gtk_list_store_set(list_store, &iter,
					   XID_COLUMN, output_info->modes[n],
					   XID_STRING_COLUMN, xid_string,
					   NAME_COLUMN, name,
					   REFRESH_COLUMN, refresh,
					   PIXCLOCK_COLUMN, pixclock,
					   PREFERRED_COLUMN,
					   n < output_info->npreferred, -1);

			free(xid_string);
			free(name);
			free(refresh);
			free(pixclock);
		}

		/* Create a view */
		tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(list_store));
		g_signal_connect(tree, "row-activated",
				 G_CALLBACK(row_activated), &res->outputs[k]);

		/* The view now holds a reference.  We can get rid of our own
		 * reference */
		g_object_unref(G_OBJECT(list_store));

		renderer = gtk_cell_renderer_text_new();
		g_object_set(G_OBJECT(renderer), "foreground", "red", NULL);
		column = gtk_tree_view_column_new_with_attributes("XID",
								  renderer,
								  "text",
								  XID_STRING_COLUMN,
								  NULL);
		gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

		renderer = gtk_cell_renderer_toggle_new();
		g_object_set(G_OBJECT(renderer), "radio", TRUE, NULL);
		column = gtk_tree_view_column_new_with_attributes("Preferred",
								  renderer,
								  "active",
								  PREFERRED_COLUMN,
								  NULL);
		gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

		renderer = gtk_cell_renderer_text_new();
		column = gtk_tree_view_column_new_with_attributes("Mode",
								  renderer,
								  "text",
								  NAME_COLUMN,
								  NULL);
		gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

		column = gtk_tree_view_column_new_with_attributes("Refresh",
								  renderer,
								  "text",
								  REFRESH_COLUMN,
								  NULL);
		gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

		column = gtk_tree_view_column_new_with_attributes("Pixclock",
								  renderer,
								  "text",
								  PIXCLOCK_COLUMN,
								  NULL);
		gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

		asprintf(&label, "%s(%s)", output_info->name, modelname);

		gtk_notebook_append_page(GTK_NOTEBOOK(notebook), tree,
					 gtk_label_new(label));

		free(label);
	}

	gtk_widget_show_all(window);
}

int main(int argc, char **argv)
{
	GtkApplication *app;
	int status;

	app = gtk_application_new("org.gtk.example", G_APPLICATION_FLAGS_NONE);
	g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
	status = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);

	return status;
}
