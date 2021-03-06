<?php

/**
 * @param $entities
 * @param $type
 */
function devshop_projects_entity_load($entities, $type) {

  if ($type == 'node') {

    $types = array(
      'site',
      'platform',
      'task',
    );

    foreach ($entities as &$entity) {
      if (in_array($entity->type, $types)) {

        // Load project and environment
        if ($entity->type == 'task') {

          // If this is a Verify Project task, we don't want to load anything.
          if (strpos($entity->title, 'Verify Project:') === 0) {
            $entity->project = $entity->rid;
            $entity->environment = NULL;
            continue;
          }

          $nid = $entity->rid;
        }
        else {
          $nid = $entity->nid;
        }

        $query = db_query("
SELECT
  project_nid, name, n.title as project_name FROM {hosting_devshop_project_environment} e 
  LEFT JOIN {node} n ON e.project_nid = n.nid
  WHERE site  = :site
    OR platform = :platform
    OR project_nid = :project
", array(
          ':site' => $nid,
          ':platform' => $nid,
          ':project' => $nid,
        ));

        $result = $query->fetch();

        // Only load the project node if there is no task
        if ($entity->type == 'task' && !empty($result)) {
          $entity->project = $result->project_nid;
          $entity->project_name = $result->project_name;
          $entity->environment = $result->name;
        }
        else if (isset($result->project_nid)) {
          $project_node = node_load($result->project_nid);
          if (!empty($project_node->project)) {
            $entity->project = $project_node->project;
            if (isset($project_node->project->environments[$result->name])) {
              $entity->environment = $project_node->project->environments[$result->name];
            }
          }
        }
      }
    }
  }
}

/**
 * Load a project object.
 */
function devshop_project_load($node) {

  if (empty($node->nid) || $node->type != 'project') {
    return;
  }

  // Load project data
  $project = (object) db_query('SELECT * FROM {hosting_devshop_project} WHERE nid = :nid', array(':nid' => $node->nid))->fetchAssoc();

  // Load all project tasks.
  $project->tasks = db_query('SELECT * FROM {hosting_task} WHERE rid = :rid ORDER BY vid DESC', array(':rid' => $node->nid))->fetchAllAssoc('task_type');

  // Load verification status of project node.
  if (!empty($project->tasks['verify'])) {
    $project->verify = $project->tasks['verify'];
  }
  
  // Load up all project settings.
  $project->name = $node->project_name;
  $project->settings = (object) unserialize($project->settings);
  
  
  // Create "refs" array to determine ref type.
  $project->settings->git['refs'] = array();
  foreach ($project->settings->git as $type => $refs) {
    if (is_array($refs)) {
      foreach ($refs as $ref) {
        $project->settings->git['refs'][$ref] = ($type == 'branches')? 'branch': 'tag';
      }
    }
  }

  // Git Repo Host
  if (strpos($project->git_url, 'github.com') !== FALSE) {
    $project->git_provider = 'github';
    $project->git_repo_url = strtr($project->git_url, array(
      'git@github.com:' => 'http://github.com/',
      'git://' => 'http://',
      '.git' => '',
    ));
  }
  elseif (strpos($project->git_url, variable_get('bitbucket_repo_url_trigger_word', 'bitbucket.org')) !== FALSE) {
    $project->git_provider = 'bitbucket';
    $project->git_repo_url = strtr($project->git_url, array(
      'git@bitbucket.com:' => 'http://bitbucket',
      'git://' => 'http://',
      '.git' => '',
    ));
  }
  else {
    $project->git_provider = 'git';
    $project->git_repo_url = '';
  }
  // @TODO: Set git providers for most common here, then add a hook to detect.

  // Load Environments
  // @TODO: Remove environments where the site has been deleted.
  $environment_data = db_query("
      SELECT
          e.*,
          s.status as site_status,
          p.status as platform_status,
          p.nid as platform,
          p.publish_path as root,
          p.publish_path as publish_path,
          g.git_ref as git_ref,
          g.repo_url as git_url,
          g.repo_path as repo_path,
          http.title as remote_host,
          sn.title as uri,
          http.title as web_server,
          http.nid as web_server_nid,
          db.title as db_server,
          db.nid as db_server_nid,
          s.db_name,
          s.profile as install_profile,
          pn.title as project_name,
          sn.created as created,
          s.vid,
          sn.title as system_domain,
          a.redirection as redirect_domain,
          e.last_task as last_task_nid
        FROM {hosting_devshop_project_environment} e
        LEFT JOIN {hosting_site} s ON e.site = s.nid
        LEFT JOIN {node} sn ON s.vid = sn.vid
        LEFT JOIN {hosting_platform} p ON s.platform = p.nid OR e.platform = p.nid
        LEFT JOIN {hosting_git} g ON p.nid = g.nid
        LEFT JOIN {node} http ON p.web_server = http.nid
        LEFT JOIN {node} db ON s.db_server = db.nid
        LEFT JOIN {node} pn ON e.project_nid = pn.nid
        LEFT JOIN {hosting_site_alias} a ON a.vid = s.vid
        WHERE project_nid = :nid AND
        e.name != ''
        ORDER BY
          name;   ", array(
    ':nid' => $node->nid
  ));
  foreach ($environment_data as $environment) {

    // Don't load the environment if the site and/or platform is deleted.
    if ($environment->site_status == HOSTING_SITE_DELETED && $environment->platform_status == HOSTING_PLATFORM_DELETED) {
      continue;
    }
    if ($environment->platform_status == HOSTING_PLATFORM_DELETED) {
      continue;
    }

    // Drush alias
    $environment->drush_alias = "@{$project->name}.{$environment->name}";

    // Unserialize environment settings.
    $environment->settings = (object) unserialize($environment->settings);

    // Get last task
    $environment->last_task_node = node_load($environment->last_task_nid);

    // Get all tasks, keyed by type.
    $environment->tasks = devshop_get_tasks($environment);

    // Create tasks list, keyed by VID
    $environment->tasks_list = array();
    foreach ($environment->tasks as $task_type => $tasks) {
      foreach ($tasks as $task) {
        $environment->tasks_list[$task->nid] = $task;
      }
    }
    krsort($environment->tasks_list);

    // The URL that the branch or tag links to.
    if ($project->git_provider == 'github') {
      $environment->git_ref_url = $project->git_repo_url . '/tree/' . $environment->git_ref;
    }
    else {
      $environment->git_ref_url = url("node/$environment->site/logs/commits");
    }

    // Environment Drupal version.
    $iid = db_query("SELECT iid FROM {hosting_package_instance} i left join {hosting_package} p on p.nid=i.package_id WHERE p.package_type = :type AND i.rid = :nid", array(':type' => 'platform', ':nid' => $environment->platform))->fetchField();
    $release = hosting_package_instance_load($iid);

    if (isset($release->version)) {
      $environment->version = $release->version;
    }
    else {
      $environment->version = t('unknown');
    }

    // @TODO: Make "servers" more abstract so we can add many more.
    // HTTP Server
    $environment->servers['http'] = array(
      'nid' => $environment->web_server_nid,
      'name' => $environment->web_server,
    );

    // On project create, there is no site, so there is no db_server.
    if (empty($environment->db_server)) {
      $db_servers = hosting_get_servers('db');
      // Project wizard saves db_server to "settings". If that doesn't exist, just use the default db server.
      if (empty($environment->settings->db_server)) {
        $environment->settings->db_server = $environment->db_server_nid = key($db_servers);
      }
      else {
        $environment->db_server_nid = $environment->settings->db_server;
      }
      
      if (!empty($db_servers[$environment->db_server_nid])) {
        $environment->db_server = $db_servers[$environment->db_server_nid];
      }
    }

    // DB Server
    $environment->servers['db'] = array(
      'nid' => $environment->db_server_nid,
      'name' => $environment->db_server,
    );

    // Web Server IP
    $server = node_load($environment->web_server_nid);
    if (!empty($server)) {
      $environment->ip_addresses = $server->ip_addresses;
    }
    else {
      $environment->ip_addresses = array();
    }

    // Load Solr if present
    if (module_exists('hosting_solr')) {
      $server_nid = db_query("SELECT server FROM {hosting_solr} WHERE nid = :nid", array(':nid' => $environment->site))->fetchField();

      $servers = hosting_get_servers('solr');
      if ($server_nid && isset($servers[$server_nid])) {
        $environment->servers['solr'] = array(
          'nid' => $server_nid,
          'name' => $servers[$server_nid],
        );
      }
    }

    // Git information.
    $environment->git_ref_stored = $environment->git_ref;
    if (isset($environment->repo_path) && file_exists($environment->repo_path . '/.git')) {

      // Timestamp of last commit.
      $environment->git_last = shell_exec("cd {$environment->repo_path}; git log --pretty=format:'%ar' --max-count=1");

      // The last commit.
      $environment->git_commit = shell_exec("cd {$environment->repo_path}; git -c color.ui=always log --max-count=1");

      // Get the exact SHA
      $environment->git_sha = trim(shell_exec("cd {$environment->repo_path}; git rev-parse HEAD  2> /dev/null"));

      // Determine the type of git ref the stored version is
      $stored_git_ref_type = $project->settings->git['refs'][$environment->git_ref_stored];
      $stored_git_sha =  trim(shell_exec("cd {$environment->repo_path}; git rev-parse {$environment->git_ref_stored} 2> /dev/null"));

      // Get the actual tag or branch. If a branch and tag have the same SHA, the tag will be output here.
      // "2> /dev/null" ensures errors don't get printed like "fatal: no tag exactly matches".
      $environment->git_ref = trim(str_replace('refs/heads/', '', shell_exec("cd {$environment->repo_path}; git describe --tags --exact-match 2> /dev/null || git symbolic-ref -q HEAD 2> /dev/null")));

      $environment->git_ref_type = $project->settings->git['refs'][$environment->git_ref];

      // If the git sha for stored branch are the same, but the type is different, detect if HEAD is detached so we know if this is on a branch or a tag.
      if ($stored_git_sha == $environment->git_sha && $stored_git_ref_type != $environment->git_ref_type) {
        $git_status = shell_exec("cd {$environment->repo_path}; git status");
        if (strpos($git_status, 'On branch ') === 0) {
          $environment->git_ref_type = 'branch';
          $environment->git_ref = $environment->git_ref_stored;
        }
        else {
          $environment->git_ref_type = 'tag';
        }
      }

      // Get git status.
      $environment->git_status = trim(shell_exec("cd {$environment->repo_path}; git -c color.ui=always  status"));

      // Limit status to 1000 lines
      $lines = explode("\n", $environment->git_status);
      $count = count($lines);
      if ($count > 100) {
        $lines = array_slice($lines, 0, 100);
        $lines[] = "# STATUS TRUNCATED. SHOWING 100 of $count LINES.";
      }
      $environment->git_status  = implode("\n", $lines);

      // Get git diff.
      $environment->git_diff = trim(shell_exec("cd {$environment->repo_path}; git -c color.ui=always diff"));

      // Limit git diff to 1000 lines
      $lines = explode("\n", $environment->git_diff);
      $count = count($lines);
      if ($count > 1000) {
        $lines = array_slice($lines, 0, 1000);
        $lines[] = "# DIFF TRUNCATED. SHOWING 1000 of $count LINES.";
      }
      $environment->git_diff  = implode("\n", $lines);

    }
    else {
      $environment->git_last = '';
      $environment->git_commit = '';
      $environment->git_sha = '';
      $environment->git_status = '';
      $environment->git_diff = '';
    }

    // Load encryption status
    if (module_exists('hosting_ssl')) {
      $environment->ssl_enabled  = db_query('SELECT ssl_enabled FROM {hosting_ssl_site} WHERE nid = :nid', array(':nid' => $environment->site))->fetchField();
    }
    elseif (module_exists('hosting_https')) {
      $environment->ssl_enabled  = db_query('SELECT https_enabled FROM {hosting_https_site} WHERE nid = :nid', array(':nid' => $environment->site))->fetchField();
    }
    else {
      $environment->ssl_enabled = FALSE;
    }

    // Environments URLs
    // Get aegir "aliases"
    $environment->domains = hosting_alias_get_aliases($environment);
    $environment->domains[] = $environment->system_domain;
    $protocol = $environment->ssl_enabled ? 'https://' : 'http://';
    if ($environment->redirect_domain) {
      $environment->url = $protocol . $environment->redirect_domain;
    }
    else {
      $environment->url = $protocol . $environment->system_domain;
    }

    // System Alias
    $environment->system_alias = '@' . $environment->system_domain;

    // If per-environment hooks config is not allowed, load it from the project.
    if (isset($project->settings->deploy) && !$project->settings->deploy['allow_environment_deploy_config']) {
      $environment->settings->deploy = $project->settings->deploy['default_hooks'];
    }

    // Load Install Method and info.
    if (isset($environment->settings->install_method)) {
      $environment->install_method = $environment->settings->install_method['method'];
    }
    else {
      $environment->install_method = 'profile';
    }

    switch ($environment->install_method) {

      // Install Method: Clone
      case 'clone':
        $environment->clone_source = $environment->settings->install_method['clone_source'];

        // Load clone source for "other".
        if ($environment->clone_source == '_other') {
          $environment->clone_source = $environment->settings->install_method['clone_source_drush'];
        }

        // Load clone source nid and environment name.
        $environment->clone_source_node = db_query("SELECT nid, e.name FROM {hosting_context} h LEFT JOIN hosting_devshop_project_environment e ON h.nid = e.site WHERE h.name = :name", array(':name' => ltrim($environment->clone_source, '@')))->fetchObject();

        // If no site node was found with the selected alias, display the alias.
        if (empty($environment->clone_source_node->name)) {
          $source_label = $environment->clone_source;
        }
        else {
          $source_label = l($environment->clone_source_node->name, "node/{$environment->clone_source_node->nid}");
        }

        // Set Install Method Label
        $environment->install_method_label = t('Clone of !source', array(
          '!source' => $source_label,
        ));
        break;

      // Install Method: "Empty Database" / "Manual Install"
      case "manual":
        $environment->install_method_label = t('Manually Installed');
        break;

      // Install Method: "Import"
      case "import":
        $environment->install_method_label = t('SQL Import');
        break;

      // Install Method: "Profile"
      case "profile":
      default:

        // We don't know package ID until platform verifies.
        if ($environment->install_profile == 0 && !empty($environment->settings->install_method['profile'])) {
          $environment->install_method_label = t('Installed with !profile', array(
            '!profile' => $environment->settings->install_method['profile'],
          ));
        }
        elseif (!empty($environment->settings->install_method['profile'])) {
          $profile = node_load($environment->install_profile);
          $environment->install_method_label = t('Installed with !profile', array(
            '!profile' => l($profile->title, "node/{$environment->install_profile}"),
          ));
        }
        else {
          $environment->install_method_label = '';
        }
        break;
    }

    // @TODO: Create a "reinstall" task to replace this.
//    $environment->clone_rebuild_url = url(
//      "hosting_confirm/{$environment->site}/site_sync", array(
//        'query' => array(
//          'source' => $environment->clone_source,
//          'rebuild' => TRUE,
//        ),
//      )
//    );

    // Save to project environments collection.
    drupal_alter('devshop_environment', $environment, $project);
    $environments[$environment->name] = $environment;
  }

  // Put live environment at the top.
  if (!empty($environments) && isset($project->settings->live) && $project->settings->live['live_environment'] && isset($environments[$project->settings->live['live_environment']])) {
    $project->environments = array();
    $live_env = $project->settings->live['live_environment'];
    $project->environments[$live_env] = $environments[$live_env];
    unset($environments[$live_env]);
    $project->environments += $environments;
  }
  elseif (!empty($environments)) {
    $project->environments = $environments;
  }
  elseif (!empty($node->title)) {
    $project->environments = array();
  }

  // Make project name and status available.
  $project->name = $node->title;
  $project->status = $node->status;

  // Webhook Status
  // @TODO: Create Hosting Webhooks module.
  // @TODO: Remove devshop_pull.
  module_load_include('inc', 'devshop_pull');
  $project->webhook_url = _devshop_pull_callback_url($node);

  // Save project object be available at $node->project.
  return $project;

}
