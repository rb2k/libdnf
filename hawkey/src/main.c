#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <wordexp.h>

// libsolv
#include "solv/repo_rpmdb.h"
#include "solv/solver.h"
#include "solv/util.h"

// hawkey
#include "frepo.h"
#include "goal.h"
#include "package.h"
#include "query.h"
#include "sack.h"

#define CFG_FILE "~/.hawkey/main.config"

static void execute_print(Query q, int show_obsoletes)
{
    PackageList plist;
    PackageListIter iter;
    Package pkg;

    plist = query_run(q);
    iter = packagelist_iter_create(plist);
    while ((pkg = packagelist_iter_next(iter)) != NULL) {
	char *nvra = package_get_nvra(pkg);
	printf("found package: %s [%s]\n",
	       nvra,
	       package_get_reponame(pkg));
	if (show_obsoletes) {
	    PackageList olist = packagelist_of_obsoletes(q->sack, pkg);
	    PackageListIter oiter = packagelist_iter_create(olist);
	    Package opkg;
	    char *onvra;
	    while ((opkg = packagelist_iter_next(oiter)) != NULL) {
		onvra = package_get_nvra(opkg);
		printf("obsoleting: %s\n", onvra);
		solv_free(onvra);
	    }
	    packagelist_iter_free(oiter);
	    packagelist_free(olist);
	}
	solv_free(nvra);
    }
    packagelist_iter_free(iter);
    packagelist_free(plist);
}

static void execute_queue(Query q, Queue *job)
{
    PackageList plist;
    PackageListIter iter;
    Package pkg;

    plist = query_run(q);
    iter = packagelist_iter_create(plist);
    while ((pkg = packagelist_iter_next(iter)) != NULL) {
	queue_push2(job, SOLVER_SOLVABLE|SOLVER_INSTALL, pkg->id);
    }
    packagelist_iter_free(iter);
    packagelist_free(plist);
}

static void search_and_print(Sack sack, const char *name)
{
    Package pkg;
    PackageList plist;
    PackageListIter iter;

    plist = sack_f_by_name(sack, name);
    iter = packagelist_iter_create(plist);
    while ((pkg = packagelist_iter_next(iter)) != NULL)
       	printf("found package by name: (%s, %s, %s)\n", package_get_name(pkg),
	       package_get_evr(pkg),
	       package_get_arch(pkg));
    packagelist_iter_free(iter);
    packagelist_free(plist);

    plist = sack_f_by_summary(sack, name);
    iter = packagelist_iter_create(plist);
    while ((pkg = packagelist_iter_next(iter)) != NULL) {
	char *nvra = package_get_nvra(pkg);
       	printf("found package by summary: %s\n", nvra);
	solv_free(nvra);
    }
    packagelist_iter_free(iter);
    packagelist_free(plist);
}

static void search_filter_repos(Sack sack, const char *name) {
    Query q = query_create(sack);

    query_filter(q, KN_PKG_NAME, FT_EQ, name);
    query_filter(q, KN_PKG_REPO, FT_NEQ, SYSTEM_REPO_NAME);
    execute_print(q, 0);
    query_free(q);
}

static void search_anded(Sack sack, const char *name_substr,
			 const char *summary_substr)
{
    Query q = query_create(sack);

    query_filter(q, KN_PKG_NAME, FT_SUBSTR, name_substr);
    query_filter(q, KN_PKG_SUMMARY, FT_SUBSTR, summary_substr);
    execute_print(q, 0);
    query_free(q);
}

static void search_provides(Sack sack, const char *name,
			    const char *version)
{
    Query q = query_create(sack);

    query_filter_provides(q, FT_GT, name, version);
    execute_print(q, 0);
    query_free(q);
}

static void resolve_install(Sack sack, const char *name)
{
    Query q = query_create(sack);
    Queue job;

    queue_init(&job);

    query_filter(q, KN_PKG_NAME, FT_EQ, name);
    execute_queue(q, &job);
    assert(job.count == 2); // XXX: the search mechanism for this case is broken.
    sack_solve(sack, &job, NULL, 0);
    queue_free(&job);
    query_free(q);
}

static void updatables_query_all(Sack sack)
{
    Queue job;
    queue_init(&job);
    queue_push2(&job, SOLVER_UPDATE|SOLVER_SOLVABLE_ALL, 0);
    sack_solve(sack, &job, NULL, 0);
    queue_free(&job);
}

static void updatables_query_name(Sack sack, const char *name)
{
    Query q = query_create(sack);

    query_filter(q, KN_PKG_NAME, FT_EQ, name);
#if 0 // must stil work if enabled
    query_filter(q, KN_PKG_REPO, FT_NEQ, SYSTEM_REPO_NAME);
#endif
    query_filter_updates(q, 1);
    execute_print(q, 0);
    query_free(q);
}

static void obsoletes(Sack sack)
{
    Query q = query_create(sack);

    query_filter_obsoleting(q, 1);
    execute_print(q, 1);
    query_free(q);
}

static void
dump_goal_errors(Goal goal)
{
    for (int i = 0; i < goal_count_problems(goal); ++i) {
	char *problem = goal_describe_problem(goal, i);
	printf("%s\n", problem);
	solv_free(problem);
    }
}

static void
erase(Sack sack, const char *name)
{
    Query q = query_create(sack);
    query_filter(q, KN_PKG_NAME, FT_EQ, name);
    query_filter(q, KN_PKG_REPO, FT_EQ, SYSTEM_REPO_NAME);

    PackageList plist = query_run(q);
    if (packagelist_count(plist) < 1) {
	printf("No installed package to erase found.\n");
	goto finish;
    }
    if (packagelist_count(plist) > 1)
	printf("(more than one updatables found, selecting the first one)\n");

    Goal goal = goal_create(sack);
    goal_erase(goal, packagelist_get(plist, 0));

    if (goal_go(goal)) {
	dump_goal_errors(goal);
	goal_free(goal);
	goto finish;
    }
    packagelist_free(plist);
    plist = goal_list_erasures(goal);
    printf("erasure count: %d\n", packagelist_count(plist));
    for (int i = 0; i < packagelist_count(plist); ++i) {
	Package pkg = packagelist_get(plist, i);
	char *nvra = package_get_nvra(pkg);

	printf("erasing %s\n", nvra);
	solv_free(nvra);
    }

    goal_free(goal);
 finish:
    packagelist_free(plist);
    query_free(q);
}

static void update(Sack sack, Package pkg)
{
    Goal goal = goal_create(sack);

    if (goal_update(goal, pkg)) {
	printf("no package of that name installed, trying install\n");
	goal_install(goal, pkg);
    }
    if (goal_go(goal)) {
	dump_goal_errors(goal);
	// handle errors
	goto finish;
    }
    // handle upgrades
    PackageList plist = goal_list_upgrades(goal);
    printf("upgrade count: %d\n", packagelist_count(plist));
    for (int i = 0; i < packagelist_count(plist); ++i) {
	Package pkg = packagelist_get(plist, i);
	char *nvra = package_get_nvra(pkg);
	char *location = package_get_location(pkg);
	Package installed = goal_package_upgrades(goal, pkg);
	char *nvra_installed = package_get_nvra(installed);

	printf("upgrading: %s using %s\n", nvra, location);
	printf("\tfrom: %s\n", nvra_installed);
	printf("\tsize: %d kB\n", package_get_size(pkg) / 1024);

	solv_free(nvra_installed);
	package_free(installed);
	solv_free(location);
	solv_free(nvra);
    }
    packagelist_free(plist);
    // handle installs
    plist = goal_list_installs(goal);
    printf("install count: %d\n", packagelist_count(plist));
    for (int i = 0; i < packagelist_count(plist); ++i) {
	Package pkg = packagelist_get(plist, i);
	char *nvra = package_get_nvra(pkg);
	char *location = package_get_location(pkg);

	printf("installing: %s using %s\n", nvra, location);
	printf("\tsize: %d kB\n", package_get_size(pkg) / 1024);

	solv_free(location);
	solv_free(nvra);
    }
    packagelist_free(plist);

 finish:
    goal_free(goal);
}

static void update_local(Sack sack, const char *fn)
{
    Package pkg;

    sack_create_cmdline_repo(sack);
    pkg = sack_add_cmdline_rpm(sack, fn);
    if (pkg) {
	update(sack, pkg);
	package_free(pkg);
    }
}

static void update_remote(Sack sack, const char *name)
{
    Query q = query_create(sack);
    PackageList plist;

    query_filter(q, KN_PKG_NAME, FT_EQ, name);
    query_filter(q, KN_PKG_REPO, FT_NEQ, SYSTEM_REPO_NAME);
    query_filter_latest(q, 1);

    plist = query_run(q);
    if (plist->count == 0) {
	printf("no updatables found\n");
	goto finish;
    }
    if (packagelist_count(plist) > 1)
	printf("(more than one updatables found, selecting the first one)\n");
    update(sack, packagelist_get(plist, 0));
finish:
    packagelist_free(plist);
    query_free(q);
}

static FRepo
config_repo(const char *name, const char *md_repo, const char *md_primary_xml)
{
    FRepo repo = frepo_create();
    frepo_set_string(repo, NAME, name);
    frepo_set_string(repo, REPOMD_FN, md_repo);
    frepo_set_string(repo, PRIMARY_FN, md_primary_xml);
    return repo;
}

static void
rtrim(char *str) {
    int len = strlen(str);
    if (len > 0 && str[len - 1] == '\n')
	str[len - 1 ] = '\0';
}

int
read_repopaths(char **md_repo, char **md_primary_xml,
	       char **md_repo_updates, char **md_primary_updates_xml)
{
    wordexp_t word_vector;

    *md_repo = *md_primary_xml = *md_repo_updates = *md_primary_updates_xml = NULL;

    if (wordexp(CFG_FILE, &word_vector, 0))
	return 1;
    if (word_vector.we_wordc < 1) {
	wordfree(&word_vector);
	return 1;
    }
    const char *cfgpath = word_vector.we_wordv[0];
    FILE *f = fopen(cfgpath, "r");
    if (!f) {
	wordfree(&word_vector);
	return 1;
    }
    size_t size = 0;
    if ((getline(md_repo, &size, f) < 0) ||
	(getline(md_primary_xml, &size, f) < 0) ||
	(getline(md_repo_updates, &size, f) < 0) ||
	(getline(md_primary_updates_xml, &size, f) < 0)) {
	solv_free(*md_repo);
	solv_free(*md_primary_xml);
	solv_free(*md_repo_updates);
	solv_free(*md_primary_updates_xml);
	fclose(f);
	wordfree(&word_vector);
	return 1;
    }
    fclose(f);
    wordfree(&word_vector);
    rtrim(*md_repo);
    rtrim(*md_primary_xml);
    rtrim(*md_repo_updates);
    rtrim(*md_primary_updates_xml);
    return 0;
}

int main(int argc, const char **argv)
{
    Sack sack = sack_create();
    FRepo repo;
    char *md_repo;
    char *md_primary_xml;
    char *md_repo_updates;
    char *md_primary_updates_xml;

    if (read_repopaths(&md_repo, &md_primary_xml, &md_repo_updates,
		       &md_primary_updates_xml)) {
	fprintf(stderr,
		"This is hawkey testing hack, it needs a readable %s file "
		"containing the following paths on separate lines:\n"
		"<main repomd.xml path>\n"
		"<main primary.xml.gz path>\n"
		"<updates repomd.xml path>\n"
		"<updates primary.xml.gz path>\n", CFG_FILE);
	return 1;
    }
    /* rpmdb */
    sack_load_rpm_repo(sack);
    /* Fedora repo */
    repo = config_repo("Fedora", md_repo, md_primary_xml);
    sack_load_yum_repo(sack, repo);
    frepo_free(repo);
    /* Fedora updates repo */
    repo = config_repo("updates", md_repo_updates, md_primary_updates_xml);
    sack_load_yum_repo(sack, repo);
    frepo_free(repo);

    if (argc == 2 && !strcmp(argv[1], "-u")) {
	updatables_query_all(sack);
    } else if (argc == 2 && !strcmp(argv[1], "-o")) {
	obsoletes(sack);
    }  else if (argc == 2) {
	search_and_print(sack, argv[1]);
    } else if (argc == 3 && !strcmp(argv[1], "-r")) {
	search_filter_repos(sack, argv[2]);
    } else if (argc == 3 && !strcmp(argv[1], "-s")) {
	resolve_install(sack, argv[2]);
    } else if (argc == 3 && !strcmp(argv[1], "-u")) {
	updatables_query_name(sack, argv[2]);
    } else if (argc == 3 && !strcmp(argv[1], "-ul")) {
	update_local(sack, argv[2]);
    } else if (argc == 3 && !strcmp(argv[1], "-ur")) {
	update_remote(sack, argv[2]);
    } else if (argc == 3 && !strcmp(argv[1], "-e")) {
	erase(sack, argv[2]);
    } else if (argc == 3) {
	search_anded(sack, argv[1], argv[2]);
    } else if (argc == 4 && !strcmp(argv[1], "-p")) {
	search_provides(sack, argv[2], argv[3]);
    }

    sack_write_all_repos(sack);
    sack_free(sack);

    return 0;
}
