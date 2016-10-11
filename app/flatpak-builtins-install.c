/*
 * Copyright © 2014 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <glib/gi18n.h>

#include <gio/gunixinputstream.h>

#include "libglnx/libglnx.h"

#include "flatpak-builtins.h"
#include "flatpak-utils.h"
#include "lib/flatpak-error.h"
#include "flatpak-chain-input-stream.h"

static char *opt_arch;
static char **opt_gpg_file;
static char **opt_subpaths;
static gboolean opt_no_pull;
static gboolean opt_no_deploy;
static gboolean opt_no_related;
static gboolean opt_runtime;
static gboolean opt_app;
static gboolean opt_bundle;
static gboolean opt_from;

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, N_("Arch to install for"), N_("ARCH") },
  { "no-pull", 0, 0, G_OPTION_ARG_NONE, &opt_no_pull, N_("Don't pull, only install from local cache"), NULL },
  { "no-deploy", 0, 0, G_OPTION_ARG_NONE, &opt_no_deploy, N_("Don't deploy, only download to local cache"), NULL },
  { "no-related", 0, 0, G_OPTION_ARG_NONE, &opt_no_related, N_("Don't install related refs"), NULL },
  { "runtime", 0, 0, G_OPTION_ARG_NONE, &opt_runtime, N_("Look for runtime with the specified name"), NULL },
  { "app", 0, 0, G_OPTION_ARG_NONE, &opt_app, N_("Look for app with the specified name"), NULL },
  { "bundle", 0, 0, G_OPTION_ARG_NONE, &opt_bundle, N_("Install from local bundle file"), NULL },
  { "from", 0, 0, G_OPTION_ARG_NONE, &opt_from, N_("Load options from file"), NULL },
  { "gpg-file", 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &opt_gpg_file, N_("Check bundle signatures with GPG key from FILE (- for stdin)"), N_("FILE") },
  { "subpath", 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &opt_subpaths, N_("Only install this subpath"), N_("PATH") },
  { NULL }
};

static GBytes *
read_gpg_data (GCancellable *cancellable,
               GError      **error)
{
  g_autoptr(GInputStream) source_stream = NULL;
  guint n_keyrings = 0;
  g_autoptr(GPtrArray) streams = NULL;

  if (opt_gpg_file != NULL)
    n_keyrings = g_strv_length (opt_gpg_file);

  guint ii;

  streams = g_ptr_array_new_with_free_func (g_object_unref);

  for (ii = 0; ii < n_keyrings; ii++)
    {
      GInputStream *input_stream = NULL;

      if (strcmp (opt_gpg_file[ii], "-") == 0)
        {
          input_stream = g_unix_input_stream_new (STDIN_FILENO, FALSE);
        }
      else
        {
          g_autoptr(GFile) file = g_file_new_for_path (opt_gpg_file[ii]);
          input_stream = G_INPUT_STREAM (g_file_read (file, cancellable, error));

          if (input_stream == NULL)
            return NULL;
        }

      /* Takes ownership. */
      g_ptr_array_add (streams, input_stream);
    }

  /* Chain together all the --keyring options as one long stream. */
  source_stream = (GInputStream *) flatpak_chain_input_stream_new (streams);

  return flatpak_read_stream (source_stream, FALSE, error);
}

static gboolean
install_bundle (FlatpakDir *dir,
                GOptionContext *context,
                int argc, char **argv,
                GCancellable *cancellable,
                GError **error)
{
  g_autoptr(GFile) file = NULL;
  const char *filename;
  g_autoptr(GBytes) gpg_data = NULL;

  if (argc < 2)
    return usage_error (context, _("Bundle filename must be specified"), error);

  if (argc > 2)
    return usage_error (context, _("Too many arguments"), error);

  filename = argv[1];

  file = g_file_new_for_commandline_arg (filename);


  if (opt_gpg_file != NULL)
    {
      /* Override gpg_data from file */
      gpg_data = read_gpg_data (cancellable, error);
      if (gpg_data == NULL)
        return FALSE;
    }

  if (!flatpak_dir_install_bundle (dir, file, gpg_data, NULL,
                                   cancellable, error))
    return FALSE;

  return TRUE;
}

static gboolean
do_install (FlatpakDir          *dir,
            gboolean             no_pull,
            gboolean             no_deploy,
            const char          *ref,
            const char          *remote_name,
            const char         **opt_subpaths,
            GCancellable        *cancellable,
            GError             **error)
{
  g_autoptr(GPtrArray) related = NULL;
  const char *slash;
  int i;

  slash = strchr (ref, '/');
  g_print (_("Installing: %s\n"), slash + 1);

  if (!flatpak_dir_install (dir,
                            opt_no_pull,
                            opt_no_deploy,
                            ref, remote_name,
                            (const char **)opt_subpaths,
                            NULL,
                            cancellable, error))
    return FALSE;

  if (!opt_no_related)
    {
      g_autoptr(GError) local_error = NULL;

      if (opt_no_pull)
        related = flatpak_dir_find_local_related (dir, ref, remote_name, NULL, &local_error);
      else
        related = flatpak_dir_find_remote_related (dir, ref, remote_name, NULL, &local_error);
      if (related == NULL)
        {
          g_printerr (_("Warning: Problem looking for related refs: %s\n"), local_error->message);
          g_clear_error (&local_error);
        }
      else
        {
          for (i = 0; i < related->len; i++)
            {
              FlatpakRelated *rel = g_ptr_array_index (related, i);
              g_auto(GStrv) parts = NULL;

              if (!rel->download)
                continue;

              parts = g_strsplit (rel->ref, "/", 0);

              g_print (_("Installing related: %s\n"), parts[1]);

              if (!flatpak_dir_install_or_update (dir,
                                                  opt_no_pull,
                                                  opt_no_deploy,
                                                  rel->ref, remote_name,
                                                  (const char **)rel->subpaths,
                                                  NULL,
                                                  cancellable, &local_error))
                {
                  g_printerr (_("Warning: Failed to install related ref: %s\n"),
                              rel->ref);
                  g_clear_error (&local_error);
                }
            }
        }
    }

  return TRUE;
}

static gboolean
install_from (FlatpakDir *dir,
              GOptionContext *context,
              int argc, char **argv,
              GCancellable *cancellable,
              GError **error)
{
  g_autoptr(GFile) file = NULL;
  g_autoptr(GBytes) file_data = NULL;
  g_autofree char *data = NULL;
  gsize data_len;
  const char *filename;
  g_autofree char *remote = NULL;
  g_autofree char *ref = NULL;
  g_auto(GStrv) parts = NULL;
  const char *slash;
  FlatpakDir *clone;

  if (argc < 2)
    return usage_error (context, _("Filename must be specified"), error);

  if (argc > 2)
    return usage_error (context, _("Too many arguments"), error);

  filename = argv[1];

  file = g_file_new_for_commandline_arg (filename);

  if (!g_file_load_contents (file, cancellable, &data, &data_len, NULL, error))
      return FALSE;

  file_data = g_bytes_new_take (g_steal_pointer (&data), data_len);

  if (!flatpak_dir_create_remote_for_ref_file (dir, file_data, &remote, &ref, error))
    return FALSE;

  /* Need to pick up the new config, in case it was applied in the system helper. */
  clone = flatpak_dir_clone (dir);
  if (!flatpak_dir_ensure_repo (clone, cancellable, error))
    return FALSE;

  slash = strchr (ref, '/');
  g_print (_("Installing: %s\n"), slash + 1);

  if (!do_install (clone,
                   opt_no_pull,
                   opt_no_deploy,
                   ref, remote,
                   (const char **)opt_subpaths,
                   cancellable, error))
    return FALSE;

  return TRUE;
}

static gboolean
looks_like_branch (const char *branch)
{
  /* In particular, / is not a valid branch char, so
     this lets us distinguish full or partial refs as
     non-branches. */
  if (!flatpak_is_valid_branch (branch, NULL))
    return FALSE;

  /* Dots are allowed in branches, but not really used much, while
     they are required for app ids, so thats a good check to
     distinguish the two */
  if (strchr (branch, '.') != NULL)
    return FALSE;

  return TRUE;
}

gboolean
flatpak_builtin_install (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(FlatpakDir) dir = NULL;
  const char *repository;
  char **prefs = NULL;
  int i, n_prefs;
  g_autofree char *default_branch = NULL;
  FlatpakKinds kinds;
  g_autoptr(GPtrArray) refs = NULL;

  context = g_option_context_new (_("REPOSITORY REF... - Install applications or runtimes"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv, 0, &dir, cancellable, error))
    return FALSE;

  if (opt_bundle)
    return install_bundle (dir, context, argc, argv, cancellable, error);

  if (opt_from)
    return install_from (dir, context, argc, argv, cancellable, error);

  if (argc < 3)
    return usage_error (context, _("REPOSITORY and REF must be specified"), error);

  if (argc > 4)
    return usage_error (context, _("Too many arguments"), error);

  repository = argv[1];
  prefs = &argv[2];
  n_prefs = argc - 2;

  /* Backwards compat for old "REPOSITORY NAME [BRANCH]" argument version */
  if (argc == 4 && looks_like_branch (argv[3]))
    {
      default_branch = g_strdup (argv[3]);
      n_prefs = 1;
    }

  kinds = flatpak_kinds_from_bools (opt_app, opt_runtime);

  refs = g_ptr_array_new_with_free_func (g_free);
  for (i = 0; i < n_prefs; i++)
    {
      const char *pref = prefs[i];
      FlatpakKinds matched_kinds;
      g_autofree char *id = NULL;
      g_autofree char *arch = NULL;
      g_autofree char *branch = NULL;
      FlatpakKinds kind;
      g_autofree char *ref = NULL;

      if (!flatpak_split_partial_ref_arg (pref, kinds, opt_arch, default_branch,
                                          &matched_kinds, &id, &arch, &branch, error))
        return FALSE;

      default_branch = flatpak_dir_get_remote_default_branch (dir, repository);
      ref = flatpak_dir_find_remote_ref (dir, repository, id, branch, default_branch, arch,
                                         matched_kinds, &kind, cancellable, error);
      if (ref == NULL)
        return FALSE;

      g_ptr_array_add (refs, g_steal_pointer (&ref));
    }

  for (i = 0; i < refs->len; i++)
    {
      const char *ref = g_ptr_array_index (refs, i);
      if (!do_install (dir,
                       opt_no_pull,
                       opt_no_deploy,
                       ref, repository,
                       (const char **)opt_subpaths,
                       cancellable, error))
        return FALSE;
    }

  return TRUE;
}

gboolean
flatpak_complete_install (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(FlatpakDir) dir = NULL;
  FlatpakKinds kinds;
  int i;

  context = g_option_context_new ("");
  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv, 0, &dir, NULL, NULL))
    return FALSE;

  kinds = flatpak_kinds_from_bools (opt_app, opt_runtime);

  switch (completion->argc)
    {
    case 0:
    case 1: /* REMOTE */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);
      flatpak_complete_options (completion, user_entries);

      {
        g_auto(GStrv) remotes = flatpak_dir_list_remotes (dir, NULL, NULL);
        if (remotes == NULL)
          return FALSE;
        for (i = 0; remotes[i] != NULL; i++)
          flatpak_complete_word (completion, "%s ", remotes[i]);
      }

      break;

    default: /* REF */
      flatpak_complete_partial_ref (completion, kinds, opt_arch, dir, completion->argv[1]);
      break;
    }

  return TRUE;
}
