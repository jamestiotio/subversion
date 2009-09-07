/*
 * delete.c:  wrappers around wc delete functionality.
 *
 * ====================================================================
 *    Licensed to the Subversion Corporation (SVN Corp.) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The SVN Corp. licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */

/* ==================================================================== */



/*** Includes. ***/

#include <apr_file_io.h>
#include "svn_types.h"
#include "svn_pools.h"
#include "svn_wc.h"
#include "svn_client.h"
#include "svn_error.h"
#include "svn_dirent_uri.h"
#include "svn_path.h"
#include "client.h"

#include "private/svn_wc_private.h"

#include "svn_private_config.h"


/*** Code. ***/


/* An svn_wc_status_func4_t callback function for finding
   status structures which are not safely deletable. */
static svn_error_t *
find_undeletables(void *baton,
                  const char *path,
                  const svn_wc_status2_t *status,
                  apr_pool_t *pool)
{
  /* Check for error-ful states. */
  if (status->text_status == svn_wc_status_obstructed)
    return svn_error_createf(SVN_ERR_NODE_UNEXPECTED_KIND, NULL,
                             _("'%s' is in the way of the resource "
                               "actually under version control"),
                             svn_dirent_local_style(path, pool));
  else if (! status->entry)
    return svn_error_createf(SVN_ERR_UNVERSIONED_RESOURCE, NULL,
                             _("'%s' is not under version control"),
                             svn_dirent_local_style(path, pool));

  else if ((status->text_status != svn_wc_status_normal
            && status->text_status != svn_wc_status_deleted
            && status->text_status != svn_wc_status_missing)
           ||
           (status->prop_status != svn_wc_status_none
            && status->prop_status != svn_wc_status_normal))
    return svn_error_createf(SVN_ERR_CLIENT_MODIFIED, NULL,
                             _("'%s' has local modifications -- commit or "
                               "revert them first"),
                             svn_dirent_local_style(path, pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_client__can_delete(const char *path,
                       svn_client_ctx_t *ctx,
                       apr_pool_t *pool)
{
  svn_opt_revision_t revision;
  revision.kind = svn_opt_revision_unspecified;

  /* Use an infinite-depth status check to see if there's anything in
     or under PATH which would make it unsafe for deletion.  The
     status callback function find_undeletables() makes the
     determination, returning an error if it finds anything that shouldn't
     be deleted. */
  return svn_client_status5(NULL, path, &revision, find_undeletables, NULL,
                            svn_depth_infinity, FALSE, FALSE, FALSE, FALSE,
                            NULL, ctx, pool);
}


static svn_error_t *
path_driver_cb_func(void **dir_baton,
                    void *parent_baton,
                    void *callback_baton,
                    const char *path,
                    apr_pool_t *pool)
{
  const svn_delta_editor_t *editor = callback_baton;
  *dir_baton = NULL;
  return editor->delete_entry(path, SVN_INVALID_REVNUM, parent_baton, pool);
}


static svn_error_t *
delete_urls(svn_commit_info_t **commit_info_p,
            const apr_array_header_t *paths,
            const apr_hash_t *revprop_table,
            svn_client_ctx_t *ctx,
            apr_pool_t *pool)
{
  svn_ra_session_t *ra_session;
  const svn_delta_editor_t *editor;
  void *edit_baton;
  void *commit_baton;
  const char *log_msg;
  svn_node_kind_t kind;
  apr_array_header_t *targets;
  apr_hash_t *commit_revprops;
  svn_error_t *err;
  const char *common;
  int i;
  apr_pool_t *subpool = svn_pool_create(pool);

  /* Condense our list of deletion targets. */
  SVN_ERR(svn_dirent_condense_targets(&common, &targets, paths, TRUE, pool,
                                      pool));
  if (! targets->nelts)
    {
      const char *bname;
      svn_uri_split(common, &common, &bname, pool);
      APR_ARRAY_PUSH(targets, const char *) = bname;
    }

  /* Create new commit items and add them to the array. */
  if (SVN_CLIENT__HAS_LOG_MSG_FUNC(ctx))
    {
      svn_client_commit_item3_t *item;
      const char *tmp_file;
      apr_array_header_t *commit_items
        = apr_array_make(pool, targets->nelts, sizeof(item));

      for (i = 0; i < targets->nelts; i++)
        {
          const char *path = APR_ARRAY_IDX(targets, i, const char *);

          item = svn_client_commit_item3_create(pool);
          item->url = svn_path_join(common, path, pool);
          item->state_flags = SVN_CLIENT_COMMIT_ITEM_DELETE;
          APR_ARRAY_PUSH(commit_items, svn_client_commit_item3_t *) = item;
        }
      SVN_ERR(svn_client__get_log_msg(&log_msg, &tmp_file, commit_items,
                                      ctx, pool));
      if (! log_msg)
        {
          svn_pool_destroy(subpool);
          return SVN_NO_ERROR;
        }
    }
  else
    log_msg = "";

  SVN_ERR(svn_client__ensure_revprop_table(&commit_revprops, revprop_table,
                                           log_msg, ctx, pool));

  /* Open an RA session for the URL. Note that we don't have a local
     directory, nor a place to put temp files. */
  SVN_ERR(svn_client__open_ra_session_internal(&ra_session, common, NULL,
                                               NULL, FALSE, TRUE, ctx, pool));

  /* Verify that each thing to be deleted actually exists (to prevent
     the creation of a revision that has no changes, since the
     filesystem allows for no-op deletes). */
  for (i = 0; i < targets->nelts; i++)
    {
      const char *path = APR_ARRAY_IDX(targets, i, const char *);
      svn_pool_clear(subpool);
      path = svn_path_uri_decode(path, pool);
      APR_ARRAY_IDX(targets, i, const char *) = path;
      SVN_ERR(svn_ra_check_path(ra_session, path, SVN_INVALID_REVNUM,
                                &kind, subpool));
      if (kind == svn_node_none)
        return svn_error_createf(SVN_ERR_FS_NOT_FOUND, NULL,
                                 "URL '%s' does not exist",
                                 svn_dirent_local_style(path, pool));
    }
  svn_pool_destroy(subpool);

  /* Fetch RA commit editor */
  SVN_ERR(svn_client__commit_get_baton(&commit_baton, commit_info_p, pool));
  SVN_ERR(svn_ra_get_commit_editor3(ra_session, &editor, &edit_baton,
                                    commit_revprops,
                                    svn_client__commit_callback,
                                    commit_baton,
                                    NULL, TRUE, /* No lock tokens */
                                    pool));

  /* Call the path-based editor driver. */
  err = svn_delta_path_driver(editor, edit_baton, SVN_INVALID_REVNUM,
                              targets, path_driver_cb_func,
                              (void *)editor, pool);
  if (err)
    {
      /* At least try to abort the edit (and fs txn) before throwing err. */
      svn_error_clear(editor->abort_edit(edit_baton, pool));
      return svn_error_return(err);
    }

  /* Close the edit. */
  return editor->close_edit(edit_baton, pool);
}

svn_error_t *
svn_client__wc_delete(const char *path,
                      svn_wc_adm_access_t *adm_access,
                      svn_boolean_t force,
                      svn_boolean_t dry_run,
                      svn_boolean_t keep_local,
                      svn_wc_notify_func2_t notify_func,
                      void *notify_baton,
                      svn_client_ctx_t *ctx,
                      apr_pool_t *pool)
{

  if (!force && !keep_local)
    /* Verify that there are no "awkward" files */
    SVN_ERR(svn_client__can_delete(path, ctx, pool));

  if (!dry_run)
    /* Mark the entry for commit deletion and perform wc deletion */
    return svn_wc_delete3(path, adm_access,
                          ctx->cancel_func, ctx->cancel_baton,
                          notify_func, notify_baton, keep_local, pool);
  else
    return SVN_NO_ERROR;
}


svn_error_t *
svn_client_delete3(svn_commit_info_t **commit_info_p,
                   const apr_array_header_t *paths,
                   svn_boolean_t force,
                   svn_boolean_t keep_local,
                   const apr_hash_t *revprop_table,
                   svn_client_ctx_t *ctx,
                   apr_pool_t *pool)
{
  if (! paths->nelts)
    return SVN_NO_ERROR;

  if (svn_path_is_url(APR_ARRAY_IDX(paths, 0, const char *)))
    {
      SVN_ERR(delete_urls(commit_info_p, paths, revprop_table, ctx, pool));
    }
  else
    {
      apr_pool_t *subpool = svn_pool_create(pool);
      int i;

      for (i = 0; i < paths->nelts; i++)
        {
          svn_wc_adm_access_t *adm_access;
          const char *path = APR_ARRAY_IDX(paths, i, const char *);
          const char *parent_path;

          svn_pool_clear(subpool);
          parent_path = svn_dirent_dirname(path, subpool);

          /* See if the user wants us to stop. */
          if (ctx->cancel_func)
            SVN_ERR(ctx->cancel_func(ctx->cancel_baton));

          /* Let the working copy library handle the PATH. */
          SVN_ERR(svn_wc__adm_open_in_context(&adm_access, ctx->wc_ctx,
                                              parent_path, TRUE, 0,
                                              ctx->cancel_func,
                                              ctx->cancel_baton, subpool));
          SVN_ERR(svn_client__wc_delete(path, adm_access, force,
                                        FALSE, keep_local,
                                        ctx->notify_func2,
                                        ctx->notify_baton2,
                                        ctx, subpool));
          SVN_ERR(svn_wc_adm_close2(adm_access, subpool));
        }
      svn_pool_destroy(subpool);
    }

  return SVN_NO_ERROR;
}
