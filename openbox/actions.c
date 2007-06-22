/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   actions.h for the Openbox window manager
   Copyright (c) 2007        Dana Jansens

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   See the COPYING file for a copy of the GNU General Public License.
*/

#include "actions.h"
#include "gettext.h"
#include "grab.h"
#include "screen.h"

#include "actions/all.h"

static void     actions_definition_ref(ObActionsDefinition *def);
static void     actions_definition_unref(ObActionsDefinition *def);
static gboolean actions_interactive_begin_act(ObActionsAct *act, guint state);
static void     actions_interactive_end_act();

static ObActionsAct *interactive_act = NULL;
static guint         interactive_initial_state = 0;

struct _ObActionsDefinition {
    guint ref;

    gchar *name;

    ObActionsDataSetupFunc setup;
    ObActionsDataFreeFunc free;
    ObActionsRunFunc run;
    ObActionsInteractiveInputFunc i_input;
    ObActionsInteractiveCancelFunc i_cancel;
};

struct _ObActionsAct {
    guint ref;

    ObActionsDefinition *def;
    gpointer options;
};

static GSList *registered = NULL;


void actions_startup(gboolean reconfig)
{
    if (reconfig) return;

    action_all_startup();
}

void actions_shutdown(gboolean reconfig)
{
    actions_interactive_cancel_act();

    if (reconfig) return;

    /* free all the registered actions */
    while (registered) {
        actions_definition_unref(registered->data);
        registered = g_slist_delete_link(registered, registered);
    }
}

gboolean actions_register(const gchar *name,
                          ObActionsDataSetupFunc setup,
                          ObActionsDataFreeFunc free,
                          ObActionsRunFunc run,
                          ObActionsInteractiveInputFunc i_input,
                          ObActionsInteractiveCancelFunc i_cancel)
{
    GSList *it;
    ObActionsDefinition *def;

    g_assert(run != NULL);
    g_assert((i_input == NULL) == (i_cancel == NULL));

    for (it = registered; it; it = g_slist_next(it)) {
        def = it->data;
        if (!g_ascii_strcasecmp(name, def->name)) /* already registered */
            return FALSE;
    }

    def = g_new(ObActionsDefinition, 1);
    def->ref = 1;
    def->name = g_strdup(name);
    def->setup = setup;
    def->free = free;
    def->run = run;
    def->i_input = i_input;
    def->i_cancel = i_cancel;

    registered = g_slist_prepend(registered, def);

    return TRUE;
}

static void actions_definition_ref(ObActionsDefinition *def)
{
    ++def->ref;
}

static void actions_definition_unref(ObActionsDefinition *def)
{
    if (def && --def->ref == 0) {
        g_free(def->name);
        g_free(def);
    }
}

ObActionsAct* actions_parse_string(const gchar *name)
{
    GSList *it;
    ObActionsDefinition *def = NULL;
    ObActionsAct *act = NULL;

    /* find the requested action */
    for (it = registered; it; it = g_slist_next(it)) {
        def = it->data;
        if (!g_ascii_strcasecmp(name, def->name))
            break;
        def = NULL;
    }

    /* if we found the action */
    if (def) {
        act = g_new(ObActionsAct, 1);
        act->ref = 1;
        act->def = def;
        actions_definition_ref(act->def);
        act->options = NULL;
    } else
        g_message(_("Invalid action '%s' requested. No such action exists."),
                  name);

    return act;
}

ObActionsAct* actions_parse(ObParseInst *i,
                            xmlDocPtr doc,
                            xmlNodePtr node)
{
    gchar *name;
    ObActionsAct *act = NULL;

    if (parse_attr_string("name", node, &name)) {
        if ((act = actions_parse_string(name)))
            /* there is more stuff to parse here */
            if (act->def->setup)
                act->options = act->def->setup(i, doc, node->xmlChildrenNode);

        g_free(name);
    }

    return act;
}

gboolean actions_act_is_interactive(ObActionsAct *act)
{
    return act->def->i_cancel != NULL;
}

void actions_act_ref(ObActionsAct *act)
{
    ++act->ref;
}

void actions_act_unref(ObActionsAct *act)
{
    if (act && --act->ref == 0) {
        /* free the action specific options */
        if (act->def->free)
            act->def->free(act->options);
        /* unref the definition */
        actions_definition_unref(act->def);
        g_free(act);
    }
}

static void actions_setup_data(ObActionsData *data,
                               ObUserAction uact,
                               Time time,
                               guint state,
                               gint x,
                               gint y,
                               ObFrameContext con,
                               struct _ObClient *client)
{
    data->uact = uact;
    data->time = time;
    data->state = state;
    data->x = x;
    data->y = y;
    data->context = con;
    data->client = client;
}

void actions_run_acts(GSList *acts,
                      ObUserAction uact,
                      Time time,
                      guint state,
                      gint x,
                      gint y,
                      ObFrameContext con,
                      struct _ObClient *client)
{
    GSList *it;

    /* Don't allow saving the initial state when running things from the
       menu */
    if (uact == OB_USER_ACTION_MENU_SELECTION)
        state = 0;
    /* If x and y are < 0 then use the current pointer position */
    if (x < 0 && y < 0)
        screen_pointer_pos(&x, &y);

    for (it = acts; it; it = g_slist_next(it)) {
        ObActionsData data;
        ObActionsAct *act = it->data;
        gboolean ok = TRUE;

        actions_setup_data(&data, uact, time, state, x, y, con, client);

        if (actions_act_is_interactive(act) &&
            (!interactive_act || interactive_act->def != act->def))
        {
            ok = actions_interactive_begin_act(act, state);
        }

        /* fire the action's run function with this data */
        if (ok) {
            if (!act->def->run(&data, act->options))
                actions_interactive_end_act();
            else {
                /* make sure its interactive if it returned TRUE */
                g_assert(act->def->i_cancel && act->def->i_input);

                /* no actions are run after the interactive one */
                break;
            }
        }
    }
}

gboolean actions_interactive_act_running()
{
    return interactive_act != NULL;
}

void actions_interactive_cancel_act()
{
    if (interactive_act) {
        interactive_act->def->i_cancel(interactive_act->options);
        actions_interactive_end_act();
    }
}

static gboolean actions_interactive_begin_act(ObActionsAct *act, guint state)
{
    /* cancel the old one */
    if (interactive_act)
        actions_interactive_cancel_act();

    if (grab_keyboard()) {
        interactive_act = act;
        actions_act_ref(interactive_act);

        interactive_initial_state = state;
        return TRUE;
    }
    else
        return FALSE;
}

static void actions_interactive_end_act()
{
    if (interactive_act) {
        ungrab_keyboard();

        actions_act_unref(interactive_act);
        interactive_act = NULL;
    }
}

gboolean actions_interactive_input_event(XEvent *e)
{
    gboolean used = FALSE;
    if (interactive_act) {
        if (!interactive_act->def->i_input(interactive_initial_state, e,
                                           interactive_act->options, &used))
        {
            used = TRUE; /* if it cancelled the action then it has to of
                            been used */
            actions_interactive_end_act();
        }
    }
    return used;
}
