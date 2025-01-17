/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "object.h"

#include "git2/object.h"

#include "repository.h"

#include "commit.h"
#include "hash.h"
#include "tree.h"
#include "blob.h"
#include "oid.h"
#include "tag.h"

bool git_object__strict_input_validation = true;

extern int git_odb_hash(git_oid *out, const void *data, size_t len, git_object_t type);

typedef struct {
	const char	*str;	/* type name string */
	size_t		size;	/* size in bytes of the object structure */

	int  (*parse)(void *self, git_odb_object *obj);
	int  (*parse_raw)(void *self, const char *data, size_t size);
	void (*free)(void *self);
} git_object_def;

static git_object_def git_objects_table[] = {
	/* 0 = GIT_OBJECT__EXT1 */
	{ "", 0, NULL, NULL, NULL },

	/* 1 = GIT_OBJECT_COMMIT */
	{ "commit", sizeof(git_commit), git_commit__parse, git_commit__parse_raw, git_commit__free },

	/* 2 = GIT_OBJECT_TREE */
	{ "tree", sizeof(git_tree), git_tree__parse, git_tree__parse_raw, git_tree__free },

	/* 3 = GIT_OBJECT_BLOB */
	{ "blob", sizeof(git_blob), git_blob__parse, git_blob__parse_raw, git_blob__free },

	/* 4 = GIT_OBJECT_TAG */
	{ "tag", sizeof(git_tag), git_tag__parse, git_tag__parse_raw, git_tag__free },

	/* 5 = GIT_OBJECT__EXT2 */
	{ "", 0, NULL, NULL, NULL },
	/* 6 = GIT_OBJECT_OFS_DELTA */
	{ "OFS_DELTA", 0, NULL, NULL, NULL },
	/* 7 = GIT_OBJECT_REF_DELTA */
	{ "REF_DELTA", 0, NULL, NULL, NULL },
};

int git_object__from_raw(
	git_object **object_out,
	const char *data,
	size_t size,
	git_object_t type)
{
	git_object_def *def;
	git_object *object;
	size_t object_size;
	int error;

	assert(object_out);
	*object_out = NULL;

	/* Validate type match */
	if (type != GIT_OBJECT_BLOB && type != GIT_OBJECT_TREE && type != GIT_OBJECT_COMMIT && type != GIT_OBJECT_TAG) {
		git_error_set(GIT_ERROR_INVALID, "the requested type is invalid");
		return GIT_ENOTFOUND;
	}

	if ((object_size = git_object_size(type)) == 0) {
		git_error_set(GIT_ERROR_INVALID, "the requested type is invalid");
		return GIT_ENOTFOUND;
	}

	/* Allocate and initialize base object */
	object = git__calloc(1, object_size);
	GIT_ERROR_CHECK_ALLOC(object);
	object->cached.flags = GIT_CACHE_STORE_PARSED;
	object->cached.type = type;
	git_odb_hash(&object->cached.oid, data, size, type);

	/* Parse raw object data */
	def = &git_objects_table[type];
	assert(def->free && def->parse_raw);

	if ((error = def->parse_raw(object, data, size)) < 0) {
		def->free(object);
		return error;
	}

	git_cached_obj_incref(object);
	*object_out = object;

	return 0;
}

int git_object__from_odb_object(
	git_object **object_out,
	git_repository *repo,
	git_odb_object *odb_obj,
	git_object_t type)
{
	int error;
	size_t object_size;
	git_object_def *def;
	git_object *object = NULL;

	assert(object_out);
	*object_out = NULL;

	/* Validate type match */
	if (type != GIT_OBJECT_ANY && type != odb_obj->cached.type) {
		git_error_set(GIT_ERROR_INVALID,
			"the requested type does not match the type in the ODB");
		return GIT_ENOTFOUND;
	}

	if ((object_size = git_object_size(odb_obj->cached.type)) == 0) {
		git_error_set(GIT_ERROR_INVALID, "the requested type is invalid");
		return GIT_ENOTFOUND;
	}

	/* Allocate and initialize base object */
	object = git__calloc(1, object_size);
	GIT_ERROR_CHECK_ALLOC(object);

	git_oid_cpy(&object->cached.oid, &odb_obj->cached.oid);
	object->cached.type = odb_obj->cached.type;
	object->cached.size = odb_obj->cached.size;
	object->repo = repo;

	/* Parse raw object data */
	def = &git_objects_table[odb_obj->cached.type];
	assert(def->free && def->parse);

	if ((error = def->parse(object, odb_obj)) < 0)
		def->free(object);
	else
		*object_out = git_cache_store_parsed(&repo->objects, object);

	return error;
}

void git_object__free(void *obj)
{
	git_object_t type = ((git_object *)obj)->cached.type;

	if (type < 0 || ((size_t)type) >= ARRAY_SIZE(git_objects_table) ||
		!git_objects_table[type].free)
		git__free(obj);
	else
		git_objects_table[type].free(obj);
}

int git_object_lookup_prefix(
	git_object **object_out,
	git_repository *repo,
	const git_oid *id,
	size_t len,
	git_object_t type)
{
	git_object *object = NULL;
	git_odb *odb = NULL;
	git_odb_object *odb_obj = NULL;
	int error = 0;

	assert(repo && object_out && id);

	if (len < GIT_OID_MINPREFIXLEN) {
		git_error_set(GIT_ERROR_OBJECT, "ambiguous lookup - OID prefix is too short");
		return GIT_EAMBIGUOUS;
	}

	error = git_repository_odb__weakptr(&odb, repo);
	if (error < 0)
		return error;

	if (len > GIT_OID_HEXSZ)
		len = GIT_OID_HEXSZ;

	if (len == GIT_OID_HEXSZ) {
		git_cached_obj *cached = NULL;

		/* We want to match the full id : we can first look up in the cache,
		 * since there is no need to check for non ambiguousity
		 */
		cached = git_cache_get_any(&repo->objects, id);
		if (cached != NULL) {
			if (cached->flags == GIT_CACHE_STORE_PARSED) {
				object = (git_object *)cached;

				if (type != GIT_OBJECT_ANY && type != object->cached.type) {
					git_object_free(object);
					git_error_set(GIT_ERROR_INVALID,
						"the requested type does not match the type in ODB");
					return GIT_ENOTFOUND;
				}

				*object_out = object;
				return 0;
			} else if (cached->flags == GIT_CACHE_STORE_RAW) {
				odb_obj = (git_odb_object *)cached;
			} else {
				assert(!"Wrong caching type in the global object cache");
			}
		} else {
			/* Object was not found in the cache, let's explore the backends.
			 * We could just use git_odb_read_unique_short_oid,
			 * it is the same cost for packed and loose object backends,
			 * but it may be much more costly for sqlite and hiredis.
			 */
			error = git_odb_read(&odb_obj, odb, id);
		}
	} else {
		git_oid short_oid = {{ 0 }};

		git_oid__cpy_prefix(&short_oid, id, len);

		/* If len < GIT_OID_HEXSZ (a strict short oid was given), we have
		 * 2 options :
		 * - We always search in the cache first. If we find that short oid is
		 *	ambiguous, we can stop. But in all the other cases, we must then
		 *	explore all the backends (to find an object if there was match,
		 *	or to check that oid is not ambiguous if we have found 1 match in
		 *	the cache)
		 * - We never explore the cache, go right to exploring the backends
		 * We chose the latter : we explore directly the backends.
		 */
		error = git_odb_read_prefix(&odb_obj, odb, &short_oid, len);
	}

	if (error < 0)
		return error;

	error = git_object__from_odb_object(object_out, repo, odb_obj, type);

	git_odb_object_free(odb_obj);

	return error;
}

int git_object_lookup(git_object **object_out, git_repository *repo, const git_oid *id, git_object_t type) {
	return git_object_lookup_prefix(object_out, repo, id, GIT_OID_HEXSZ, type);
}

void git_object_free(git_object *object)
{
	if (object == NULL)
		return;

	git_cached_obj_decref(object);
}

const git_oid *git_object_id(const git_object *obj)
{
	assert(obj);
	return &obj->cached.oid;
}

git_object_t git_object_type(const git_object *obj)
{
	assert(obj);
	return obj->cached.type;
}

git_repository *git_object_owner(const git_object *obj)
{
	assert(obj);
	return obj->repo;
}

const char *git_object_type2string(git_object_t type)
{
	if (type < 0 || ((size_t) type) >= ARRAY_SIZE(git_objects_table))
		return "";

	return git_objects_table[type].str;
}

git_object_t git_object_string2type(const char *str)
{
	if (!str)
		return GIT_OBJECT_INVALID;

	return git_object_stringn2type(str, strlen(str));
}

git_object_t git_object_stringn2type(const char *str, size_t len)
{
	size_t i;

	if (!str || !len || !*str)
		return GIT_OBJECT_INVALID;

	for (i = 0; i < ARRAY_SIZE(git_objects_table); i++)
		if (*git_objects_table[i].str &&
			!git__prefixncmp(str, len, git_objects_table[i].str))
			return (git_object_t)i;

	return GIT_OBJECT_INVALID;
}

int git_object_typeisloose(git_object_t type)
{
	if (type < 0 || ((size_t) type) >= ARRAY_SIZE(git_objects_table))
		return 0;

	return (git_objects_table[type].size > 0) ? 1 : 0;
}

size_t git_object_size(git_object_t type)
{
	if (type < 0 || ((size_t) type) >= ARRAY_SIZE(git_objects_table))
		return 0;

	return git_objects_table[type].size;
}

static int dereference_object(git_object **dereferenced, git_object *obj)
{
	git_object_t type = git_object_type(obj);

	switch (type) {
	case GIT_OBJECT_COMMIT:
		return git_commit_tree((git_tree **)dereferenced, (git_commit*)obj);

	case GIT_OBJECT_TAG:
		return git_tag_target(dereferenced, (git_tag*)obj);

	case GIT_OBJECT_BLOB:
	case GIT_OBJECT_TREE:
		return GIT_EPEEL;

	default:
		return GIT_EINVALIDSPEC;
	}
}

static int peel_error(int error, const git_oid *oid, git_object_t type)
{
	const char *type_name;
	char hex_oid[GIT_OID_HEXSZ + 1];

	type_name = git_object_type2string(type);

	git_oid_fmt(hex_oid, oid);
	hex_oid[GIT_OID_HEXSZ] = '\0';

	git_error_set(GIT_ERROR_OBJECT, "the git_object of id '%s' can not be "
		"successfully peeled into a %s (git_object_t=%i).", hex_oid, type_name, type);

	return error;
}

static int check_type_combination(git_object_t type, git_object_t target)
{
	if (type == target)
		return 0;

	switch (type) {
	case GIT_OBJECT_BLOB:
	case GIT_OBJECT_TREE:
		/* a blob or tree can never be peeled to anything but themselves */
		return GIT_EINVALIDSPEC;
		break;
	case GIT_OBJECT_COMMIT:
		/* a commit can only be peeled to a tree */
		if (target != GIT_OBJECT_TREE && target != GIT_OBJECT_ANY)
			return GIT_EINVALIDSPEC;
		break;
	case GIT_OBJECT_TAG:
		/* a tag may point to anything, so we let anything through */
		break;
	default:
		return GIT_EINVALIDSPEC;
	}

	return 0;
}

int git_object_peel(
	git_object **peeled,
	const git_object *object,
	git_object_t target_type)
{
	git_object *source, *deref = NULL;
	int error;

	assert(object && peeled);

	assert(target_type == GIT_OBJECT_TAG ||
		target_type == GIT_OBJECT_COMMIT ||
		target_type == GIT_OBJECT_TREE ||
		target_type == GIT_OBJECT_BLOB ||
		target_type == GIT_OBJECT_ANY);

	if ((error = check_type_combination(git_object_type(object), target_type)) < 0)
		return peel_error(error, git_object_id(object), target_type);

	if (git_object_type(object) == target_type)
		return git_object_dup(peeled, (git_object *)object);

	source = (git_object *)object;

	while (!(error = dereference_object(&deref, source))) {

		if (source != object)
			git_object_free(source);

		if (git_object_type(deref) == target_type) {
			*peeled = deref;
			return 0;
		}

		if (target_type == GIT_OBJECT_ANY &&
			git_object_type(deref) != git_object_type(object))
		{
			*peeled = deref;
			return 0;
		}

		source = deref;
		deref = NULL;
	}

	if (source != object)
		git_object_free(source);

	git_object_free(deref);

	if (error)
		error = peel_error(error, git_object_id(object), target_type);

	return error;
}

int git_object_dup(git_object **dest, git_object *source)
{
	git_cached_obj_incref(source);
	*dest = source;
	return 0;
}

int git_object_lookup_bypath(
		git_object **out,
		const git_object *treeish,
		const char *path,
		git_object_t type)
{
	int error = -1;
	git_tree *tree = NULL;
	git_tree_entry *entry = NULL;

	assert(out && treeish && path);

	if ((error = git_object_peel((git_object**)&tree, treeish, GIT_OBJECT_TREE)) < 0 ||
		 (error = git_tree_entry_bypath(&entry, tree, path)) < 0)
	{
		goto cleanup;
	}

	if (type != GIT_OBJECT_ANY && git_tree_entry_type(entry) != type)
	{
		git_error_set(GIT_ERROR_OBJECT,
				"object at path '%s' is not of the asked-for type %d",
				path, type);
		error = GIT_EINVALIDSPEC;
		goto cleanup;
	}

	error = git_tree_entry_to_object(out, git_object_owner(treeish), entry);

cleanup:
	git_tree_entry_free(entry);
	git_tree_free(tree);
	return error;
}

int git_object_short_id(git_buf *out, const git_object *obj)
{
	git_repository *repo;
	int len = GIT_ABBREV_DEFAULT, error;
	git_oid id = {{0}};
	git_odb *odb;

	assert(out && obj);

	git_buf_sanitize(out);
	repo = git_object_owner(obj);

	if ((error = git_repository__configmap_lookup(&len, repo, GIT_CONFIGMAP_ABBREV)) < 0)
		return error;

	if ((error = git_repository_odb(&odb, repo)) < 0)
		return error;

	while (len < GIT_OID_HEXSZ) {
		/* set up short oid */
		memcpy(&id.id, &obj->cached.oid.id, (len + 1) / 2);
		if (len & 1)
			id.id[len / 2] &= 0xf0;

		error = git_odb_exists_prefix(NULL, odb, &id, len);
		if (error != GIT_EAMBIGUOUS)
			break;

		git_error_clear();
		len++;
	}

	if (!error && !(error = git_buf_grow(out, len + 1))) {
		git_oid_tostr(out->ptr, len + 1, &id);
		out->size = len;
	}

	git_odb_free(odb);

	return error;
}

bool git_object__is_valid(
	git_repository *repo, const git_oid *id, git_object_t expected_type)
{
	git_odb *odb;
	git_object_t actual_type;
	size_t len;
	int error;

	if (!git_object__strict_input_validation)
		return true;

	if ((error = git_repository_odb__weakptr(&odb, repo)) < 0 ||
		(error = git_odb_read_header(&len, &actual_type, odb, id)) < 0)
		return false;

	if (expected_type != GIT_OBJECT_ANY && expected_type != actual_type) {
		git_error_set(GIT_ERROR_INVALID,
			"the requested type does not match the type in the ODB");
		return false;
	}

	return true;
}

/* Deprecated functions */

size_t git_object__size(git_object_t type)
{
	return git_object_size(type);
}
