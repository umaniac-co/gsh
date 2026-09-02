#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "builtin_files.h"
#include "native_viewer.h"
#include "resource_protocol.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdlib.h> /* CANON-INCLUDE: macos */
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#if !defined(__APPLE__)
#include <sys/sysmacros.h> /* CANON-INCLUDE: linux */
#endif
#include <unistd.h>
#include <wchar.h>

enum {
    GSH_FILE_MEMORY_CAP = 384,
    GSH_FILE_ENTRY_CAP = 65536,
    GSH_FILE_OPERAND_CAP = 128,
    GSH_FILE_RECURSION_CAP = 512,
    GSH_FILE_LINE_CAP = PATH_MAX + 1024,
    GSH_VIEW_INDEX_CAP = 1048576,
};

typedef enum {
    LS_FORMAT_ONE,
    LS_FORMAT_COLUMNS,
    LS_FORMAT_ACROSS,
    LS_FORMAT_COMMAS,
    LS_FORMAT_LONG,
} ls_format;

typedef enum {
    LS_SORT_NAME,
    LS_SORT_SIZE,
    LS_SORT_TIME,
    LS_SORT_NONE,
    LS_SORT_NATURAL,
} ls_sort;

typedef struct {
    bool all;
    bool almost_all;
    bool classify;
    bool dereference_operands;
    bool dereference_all;
    bool recursive;
    bool directory;
    bool omit_owner;
    bool inode;
    bool kib_blocks;
    bool numeric;
    bool omit_group;
    bool slash_directories;
    bool quote_nonprintable;
    bool reverse;
    bool blocks;
    bool directory_order;
    bool time_sort_requested;
    bool time_access;
    bool time_change;
    ls_format format;
    ls_sort sort;
} ls_options;

typedef struct {
    char name[PATH_MAX];
    struct stat status;
    uint64_t sequence;
    bool metadata;
} file_record;

typedef struct {
    file_record memory[GSH_FILE_MEMORY_CAP];
    size_t count;
    int spool[2];
    bool spilled;
} file_catalog;

typedef struct {
    char path[PATH_MAX];
} recursion_item;

typedef struct {
    file_catalog catalog;
    recursion_item pending[GSH_FILE_RECURSION_CAP];
    size_t pending_count;
} file_workspace;

typedef struct {
    gsh_builtin_resource_sink *sink;
    const char *base_directory;
    bool navigable_root;
} resource_scope;

static int emit_text(const gsh_builtin_io *io, int descriptor,
                     const char *text)
{
    if (io == NULL || text == NULL) return 1;
    return gsh_builtin_output(io, descriptor, text, strlen(text));
}

static int emit_format(const gsh_builtin_io *io, int descriptor,
                       const char *format, ...)
{
    char line[GSH_FILE_LINE_CAP];
    va_list arguments;
    int length;

    if (io == NULL || format == NULL) return 1;
    va_start(arguments, format);
    length = vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    if (length < 0 || (size_t)length >= sizeof(line)) {
        errno = EOVERFLOW;
        return 1;
    }
    return gsh_builtin_output(io, descriptor, line, (size_t)length);
}

static int file_error(const gsh_builtin_io *io, const char *utility,
                      const char *path, int error)
{
    if (io == NULL || utility == NULL || path == NULL) return 1;
    return emit_format(io, STDERR_FILENO, "gsh: %s: %s: %s\n", utility,
                       path, strerror(error)) == 0 ? 1 : 1;
}

static void ls_defaults(ls_options *options)
{
    bool terminal;
    if (options == NULL) return;
    (void)memset(options, 0, sizeof(*options));
    terminal = isatty(STDOUT_FILENO);
    options->format = terminal ? LS_FORMAT_COLUMNS : LS_FORMAT_ONE;
    options->quote_nonprintable = terminal;
    options->sort = LS_SORT_NAME;
}

static void ls_long_option(ls_options *options, unsigned char option)
{
    if (options == NULL) return;
    options->format = LS_FORMAT_LONG;
    if (option == 'g') options->omit_owner = true;
    if (option == 'o') options->omit_group = true;
    if (option == 'n') options->numeric = true;
}

static bool ls_apply_format_option(ls_options *options,
                                   unsigned char option)
{
    if (options == NULL) return false;
    switch (option) {
    case 'C': options->format = LS_FORMAT_COLUMNS; break;
    case 'm': options->format = LS_FORMAT_COMMAS; break;
    case 'x': options->format = LS_FORMAT_ACROSS; break;
    case '1':
        if (options->format != LS_FORMAT_LONG) options->format = LS_FORMAT_ONE;
        break;
    case 'g': case 'l': case 'n': case 'o': ls_long_option(options, option); break;
    default: return false;
    }
    return true;
}

static bool ls_apply_traversal_option(ls_options *options,
                                      unsigned char option)
{
    if (options == NULL) return false;
    switch (option) {
    case 'A':
        if (!options->directory_order) {
            options->almost_all = true;
            options->all = false;
        }
        break;
    case 'a': options->all = true; options->almost_all = false; break;
    case 'F': options->classify = true; options->slash_directories = false; break;
    case 'p': options->slash_directories = true; options->classify = false; break;
    case 'H': options->dereference_operands = true; options->dereference_all = false; break;
    case 'L': options->dereference_all = true; options->dereference_operands = false; break;
    case 'R': options->recursive = true; options->directory = false; break;
    case 'd': options->directory = true; options->recursive = false; break;
    default: return false;
    }
    return true;
}

static bool ls_apply_detail_option(ls_options *options,
                                   unsigned char option)
{
    if (options == NULL) return false;
    switch (option) {
    case 'S': if (!options->directory_order) options->sort = LS_SORT_SIZE; break;
    case 'f':
        options->sort = LS_SORT_NONE;
        options->directory_order = true;
        options->all = true;
        options->almost_all = false;
        options->reverse = false;
        break;
    case 't':
        if (!options->directory_order) {
            options->sort = LS_SORT_TIME;
            options->time_sort_requested = true;
        }
        break;
    case 'c':
        options->time_change = true;
        options->time_access = false;
        if (options->time_sort_requested && !options->directory_order)
            options->sort = LS_SORT_TIME;
        break;
    case 'u':
        options->time_access = true;
        options->time_change = false;
        if (options->time_sort_requested && !options->directory_order)
            options->sort = LS_SORT_TIME;
        break;
    case 'r': if (options->sort != LS_SORT_NONE) options->reverse = true; break;
    case 'i': options->inode = true; break;
    case 'k': options->kib_blocks = true; break;
    case 's': options->blocks = true; break;
    case 'q': options->quote_nonprintable = true; break;
    default: return false;
    }
    return true;
}

static bool ls_apply_option(ls_options *options, unsigned char option)
{
    if (options == NULL) return false;
    return ls_apply_format_option(options, option) ||
           ls_apply_traversal_option(options, option) ||
           ls_apply_detail_option(options, option);
}

static int parse_ls_options(size_t argc, char *const argv[],
                            ls_options *options, size_t *first)
{
    size_t argument;

    if (argv == NULL || options == NULL || first == NULL) return -1;
    ls_defaults(options);
    for (argument = 1U; argument < argc; argument++) {
        size_t offset;
        const char *text = argv[argument];

        if (strcmp(text, "--") == 0) { argument++; break; }
        if (text[0] != '-' || text[1] == '\0') break;
        for (offset = 1U; text[offset] != '\0'; offset++) {
            if (!ls_apply_option(options, (unsigned char)text[offset])) {
                return -1;
            }
        }
    }
    *first = argument;
    return 0;
}

static int temporary_file(void)
{
    char pattern[] = "/tmp/gsh-files-XXXXXX";
    int descriptor = mkstemp(pattern);

    if (descriptor >= 0) {
        (void)unlink(pattern);
        (void)fcntl(descriptor, F_SETFD, FD_CLOEXEC);
    }
    return descriptor;
}

static void catalog_reset(file_catalog *catalog)
{
    if (catalog == NULL) return;
    if (catalog->spool[0] >= 0) (void)close(catalog->spool[0]);
    if (catalog->spool[1] >= 0) (void)close(catalog->spool[1]);
    (void)memset(catalog, 0, sizeof(*catalog));
    catalog->spool[0] = -1;
    catalog->spool[1] = -1;
}

static int write_record(int descriptor, size_t index,
                        const file_record *record)
{
    const char *bytes = (const char *)record;
    size_t offset = 0U;
    off_t position = (off_t)(index * sizeof(*record));

    if (descriptor < 0 || record == NULL) return -1;
    while (offset < sizeof(*record)) {
        ssize_t amount = pwrite(descriptor, bytes + offset,
                                sizeof(*record) - offset,
                                position + (off_t)offset);
        if (amount > 0) offset += (size_t)amount;
        else if (amount == -1 && errno == EINTR) continue;
        else return -1;
    }
    return 0;
}

static int read_record(int descriptor, size_t index, file_record *record)
{
    char *bytes = (char *)record;
    size_t offset = 0U;
    off_t position = (off_t)(index * sizeof(*record));

    if (descriptor < 0 || record == NULL) return -1;
    while (offset < sizeof(*record)) {
        ssize_t amount = pread(descriptor, bytes + offset,
                               sizeof(*record) - offset,
                               position + (off_t)offset);
        if (amount > 0) offset += (size_t)amount;
        else if (amount == -1 && errno == EINTR) continue;
        else return -1;
    }
    return 0;
}

static int catalog_spill(file_catalog *catalog)
{
    size_t index;

    if (catalog == NULL || catalog->spilled) return catalog == NULL ? -1 : 0;
    catalog->spool[0] = temporary_file();
    catalog->spool[1] = temporary_file();
    if (catalog->spool[0] < 0 || catalog->spool[1] < 0) return -1;
    for (index = 0U; index < catalog->count; index++) {
        if (write_record(catalog->spool[0], index,
                         &catalog->memory[index]) == -1) return -1;
    }
    catalog->spilled = true;
    return 0;
}

static int catalog_add(file_catalog *catalog, const file_record *record)
{
    if (catalog == NULL || record == NULL ||
        catalog->count >= GSH_FILE_ENTRY_CAP) {
        errno = EOVERFLOW;
        return -1;
    }
    if (!catalog->spilled && catalog->count < GSH_FILE_MEMORY_CAP) {
        catalog->memory[catalog->count++] = *record;
        return 0;
    }
    if (!catalog->spilled && catalog_spill(catalog) == -1) return -1;
    if (write_record(catalog->spool[0], catalog->count, record) == -1) {
        return -1;
    }
    catalog->count++;
    return 0;
}

static int natural_compare(const char *left, const char *right)
{
    size_t a = 0U;
    size_t b = 0U;

    if (left == NULL || right == NULL) return 0;
    while (left[a] != '\0' && right[b] != '\0') {
        if (left[a] >= '0' && left[a] <= '9' &&
            right[b] >= '0' && right[b] <= '9') {
            size_t az = a;
            size_t bz = b;
            size_t ae;
            size_t be;
            while (left[az] == '0') az++;
            while (right[bz] == '0') bz++;
            ae = az;
            be = bz;
            while (left[ae] >= '0' && left[ae] <= '9') ae++;
            while (right[be] >= '0' && right[be] <= '9') be++;
            if (ae - az != be - bz) return ae - az < be - bz ? -1 : 1;
            if (ae != az) {
                int value = memcmp(left + az, right + bz, ae - az);
                if (value != 0) return value;
            }
            a = ae;
            b = be;
            continue;
        }
        if ((unsigned char)left[a] != (unsigned char)right[b]) {
            return (unsigned char)left[a] < (unsigned char)right[b] ? -1 : 1;
        }
        a++;
        b++;
    }
    return left[a] == right[b] ? strcmp(left, right)
                               : (left[a] == '\0' ? -1 : 1);
}

static struct timespec record_time(const file_record *record,
                                   const ls_options *options)
{
    struct timespec value = {0, 0};
    if (record == NULL || options == NULL) return value;
#if defined(__APPLE__)
    if (options->time_access) {
        value.tv_sec = record->status.st_atimespec.tv_sec;
        value.tv_nsec = record->status.st_atimespec.tv_nsec;
    } else if (options->time_change) {
        value.tv_sec = record->status.st_ctimespec.tv_sec;
        value.tv_nsec = record->status.st_ctimespec.tv_nsec;
    } else {
        value.tv_sec = record->status.st_mtimespec.tv_sec;
        value.tv_nsec = record->status.st_mtimespec.tv_nsec;
    }
#else
    if (options->time_access) {
        value.tv_sec = record->status.st_atim.tv_sec;
        value.tv_nsec = record->status.st_atim.tv_nsec;
    } else if (options->time_change) {
        value.tv_sec = record->status.st_ctim.tv_sec;
        value.tv_nsec = record->status.st_ctim.tv_nsec;
    } else {
        value.tv_sec = record->status.st_mtim.tv_sec;
        value.tv_nsec = record->status.st_mtim.tv_nsec;
    }
#endif
    return value;
}

static int record_compare(const file_record *left, const file_record *right,
                          const ls_options *options)
{
    int value = 0;

    if (left == NULL || right == NULL || options == NULL) return 0;
    if (options->sort == LS_SORT_NONE) {
        value = left->sequence < right->sequence ? -1 :
                (left->sequence > right->sequence ? 1 : 0);
    } else if (options->sort == LS_SORT_SIZE &&
               left->status.st_size != right->status.st_size) {
        value = left->status.st_size > right->status.st_size ? -1 : 1;
    } else if (options->sort == LS_SORT_TIME) {
        struct timespec a = record_time(left, options);
        struct timespec b = record_time(right, options);
        if (a.tv_sec != b.tv_sec) value = a.tv_sec > b.tv_sec ? -1 : 1;
        else if (a.tv_nsec != b.tv_nsec) value = a.tv_nsec > b.tv_nsec ? -1 : 1;
    }
    if (value == 0) {
        value = options->sort == LS_SORT_NATURAL
                    ? natural_compare(left->name, right->name)
                    : strcoll(left->name, right->name);
        if (value == 0) value = strcmp(left->name, right->name);
    }
    return options->reverse ? -value : value;
}

static void sort_memory(file_catalog *catalog, const ls_options *options)
{
    size_t index;

    if (catalog == NULL || options == NULL) return;
    for (index = 1U; index < catalog->count; index++) {
        file_record item = catalog->memory[index];
        size_t position = index;

        while (position > 0U && record_compare(
                   &item, &catalog->memory[position - 1U], options) < 0) {
            catalog->memory[position] = catalog->memory[position - 1U];
            position--;
        }
        catalog->memory[position] = item;
    }
}

static int merge_pair(int source, int destination, size_t first,
                      size_t middle, size_t end, const ls_options *options)
{
    file_record left;
    file_record right;
    size_t a = first;
    size_t b = middle;
    size_t out = first;
    bool have_left = false;
    bool have_right = false;

    while (out < end) {
        if (!have_left && a < middle) {
            if (read_record(source, a, &left) == -1) return -1;
            have_left = true;
        }
        if (!have_right && b < end) {
            if (read_record(source, b, &right) == -1) return -1;
            have_right = true;
        }
        if (have_left && (!have_right ||
            record_compare(&left, &right, options) <= 0)) {
            if (write_record(destination, out++, &left) == -1) return -1;
            a++;
            have_left = false;
        } else {
            if (write_record(destination, out++, &right) == -1) return -1;
            b++;
            have_right = false;
        }
    }
    return 0;
}

static int sort_spool(file_catalog *catalog, const ls_options *options)
{
    size_t width;
    int source = 0;
    int destination = 1;

    if (catalog == NULL || options == NULL || !catalog->spilled) return -1;
    for (width = 1U; width < catalog->count; width *= 2U) {
        size_t first;
        if (ftruncate(catalog->spool[destination], 0) == -1) return -1;
        for (first = 0U; first < catalog->count; first += 2U * width) {
            size_t middle = first + width < catalog->count
                                ? first + width : catalog->count;
            size_t end = first + 2U * width < catalog->count
                             ? first + 2U * width : catalog->count;
            if (merge_pair(catalog->spool[source],
                           catalog->spool[destination], first, middle, end,
                           options) == -1) return -1;
        }
        { int swap = source; source = destination; destination = swap; }
    }
    if (source != 0) {
        int swap = catalog->spool[0];
        catalog->spool[0] = catalog->spool[1];
        catalog->spool[1] = swap;
    }
    return 0;
}

static int catalog_sort(file_catalog *catalog, const ls_options *options)
{
    if (catalog == NULL || options == NULL) return -1;
    if (options->sort == LS_SORT_NONE) return 0;
    if (catalog->spilled) return sort_spool(catalog, options);
    sort_memory(catalog, options);
    return 0;
}

static int catalog_get(const file_catalog *catalog, size_t index,
                       file_record *record)
{
    if (catalog == NULL || record == NULL || index >= catalog->count) {
        return -1;
    }
    if (catalog->spilled) return read_record(catalog->spool[0], index, record);
    *record = catalog->memory[index];
    return 0;
}

static bool visible_entry(const char *name, const ls_options *options)
{
    if (name == NULL || options == NULL) return false;
    if (name[0] != '.') return true;
    if (options->all) return true;
    return options->almost_all && strcmp(name, ".") != 0 &&
           strcmp(name, "..") != 0;
}

static int enumerate_directory(int descriptor, const ls_options *options,
                               file_catalog *catalog)
{
    DIR *directory;
    uint64_t sequence = 0U;
    size_t turns;
    int result = 0;

    if (descriptor < 0 || options == NULL || catalog == NULL) return -1;
    directory = fdopendir(descriptor);
    if (directory == NULL) { (void)close(descriptor); return -1; }
    for (turns = 0U; turns < GSH_FILE_ENTRY_CAP; turns++) {
        struct dirent *entry;
        file_record record;
        size_t length;

        errno = 0;
        entry = readdir(directory);
        if (entry == NULL) { result = errno == 0 ? 0 : -1; break; }
        if (!visible_entry(entry->d_name, options)) { sequence++; continue; }
        length = strlen(entry->d_name);
        if (length > NAME_MAX) { errno = ENAMETOOLONG; break; }
        (void)memset(&record, 0, sizeof(record));
        (void)memcpy(record.name, entry->d_name, length + 1U);
        record.sequence = sequence++;
        record.metadata = fstatat(dirfd(directory), entry->d_name,
                                  &record.status,
                                  options->dereference_all ? 0
                                                           : AT_SYMLINK_NOFOLLOW) == 0;
        if (catalog_add(catalog, &record) == -1) break;
    }
    if (turns == GSH_FILE_ENTRY_CAP) { errno = EOVERFLOW; result = -1; }
    else if (result == 0 && errno != 0) result = -1;
    {
        int saved = errno;
        (void)closedir(directory);
        errno = saved;
    }
    return result;
}

static char mode_type(mode_t mode)
{
    if (S_ISDIR(mode)) return 'd';
    if (S_ISLNK(mode)) return 'l';
    if (S_ISCHR(mode)) return 'c';
    if (S_ISBLK(mode)) return 'b';
    if (S_ISFIFO(mode)) return 'p';
    if (S_ISSOCK(mode)) return 's';
    return '-';
}

static void mode_text(mode_t mode, char text[11])
{
    static const mode_t masks[9] = {
        S_IRUSR, S_IWUSR, S_IXUSR, S_IRGRP, S_IWGRP,
        S_IXGRP, S_IROTH, S_IWOTH, S_IXOTH};
    static const char letters[3] = {'r', 'w', 'x'};
    size_t index;

    if (text == NULL) return;
    text[0] = mode_type(mode);
    for (index = 0U; index < 9U; index++) {
        text[index + 1U] = (mode & masks[index]) != 0
                               ? letters[index % 3U] : '-';
    }
    if ((mode & S_ISUID) != 0) text[3] = (mode & S_IXUSR) != 0 ? 's' : 'S';
    if ((mode & S_ISGID) != 0) text[6] = (mode & S_IXGRP) != 0 ? 's' : 'S';
    if ((mode & S_ISVTX) != 0) text[9] = (mode & S_IXOTH) != 0 ? 't' : 'T';
    text[10] = '\0';
}

static const char *owner_text(uid_t owner, bool numeric,
                              char storage[32])
{
    struct passwd *entry;

    if (storage == NULL) return "?";
    entry = numeric ? NULL : getpwuid(owner);
    if (entry != NULL) return entry->pw_name;
    (void)snprintf(storage, 32U, "%ju", (uintmax_t)owner);
    return storage;
}

static const char *group_text(gid_t group, bool numeric,
                              char storage[32])
{
    struct group *entry;

    if (storage == NULL) return "?";
    entry = numeric ? NULL : getgrgid(group);
    if (entry != NULL && entry->gr_name[0] != '\0') return entry->gr_name;
    (void)snprintf(storage, 32U, "%ju", (uintmax_t)group);
    return storage;
}

static void timestamp_text(const file_record *record,
                           const ls_options *options, char text[64])
{
    struct timespec value;
    struct tm local;
    time_t now = time(NULL);
    const char *format;

    if (record == NULL || options == NULL || text == NULL) return;
    value = record_time(record, options);
    format = value.tv_sec > now - (time_t)(180 * 24 * 60 * 60) &&
                     value.tv_sec <= now
                 ? "%b %e %H:%M" : "%b %e  %Y";
    if (localtime_r(&value.tv_sec, &local) == NULL ||
        strftime(text, 64U, format, &local) == 0U) {
        (void)snprintf(text, 64U, "%jd", (intmax_t)value.tv_sec);
    }
}

static char type_indicator(mode_t mode, const ls_options *options)
{
    if (options == NULL) return '\0';
    if (S_ISDIR(mode)) return options->classify || options->slash_directories
                                  ? '/' : '\0';
    if (!options->classify) return '\0';
    if (S_ISLNK(mode)) return '@';
    if (S_ISFIFO(mode)) return '|';
    if (S_ISSOCK(mode)) return '=';
    if (S_ISREG(mode) && (mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0) return '*';
    return '\0';
}

static size_t printable_name(const char *name, bool quote,
                             char output[PATH_MAX])
{
    mbstate_t state;
    size_t length;
    size_t source = 0U;
    size_t used = 0U;
    if (name == NULL || output == NULL) return 0U;
    length = strlen(name);
    (void)memset(&state, 0, sizeof(state));
    while (source < length && used + 1U < PATH_MAX) {
        wchar_t character;
        size_t bytes;
        if (!quote) {
            output[used++] = name[source++];
            continue;
        }
        bytes = mbrtowc(&character, name + source, length - source, &state);
        if (bytes == (size_t)-1 || bytes == (size_t)-2) {
            output[used++] = '?';
            source++;
            (void)memset(&state, 0, sizeof(state));
            continue;
        }
        if (bytes == 0U) break;
        if (!iswprint(character)) {
            output[used++] = '?';
        } else if (used + bytes < PATH_MAX) {
            (void)memcpy(output + used, name + source, bytes);
            used += bytes;
        } else {
            break;
        }
        source += bytes;
    }
    output[used] = '\0';
    return used;
}

static uintmax_t display_blocks(const struct stat *status,
                                const ls_options *options)
{
    uintmax_t blocks;

    if (status == NULL || options == NULL) return 0U;
    blocks = status->st_blocks < 0 ? 0U : (uintmax_t)status->st_blocks;
    return options->kib_blocks ? (blocks + 1U) / 2U : blocks;
}

static gsh_resource_type resource_type_for_mode(mode_t mode)
{
    if (S_ISREG(mode)) return GSH_RESOURCE_REGULAR;
    if (S_ISDIR(mode)) return GSH_RESOURCE_DIRECTORY;
    if (S_ISLNK(mode)) return GSH_RESOURCE_SYMLINK;
    return GSH_RESOURCE_UNKNOWN;
}

static size_t file_text_width(const char *text, size_t length)
{
    mbstate_t state;
    size_t offset = 0U;
    size_t width = 0U;
    if (text == NULL) return 0U;
    (void)memset(&state, 0, sizeof(state));
    while (offset < length) {
        wchar_t character;
        size_t bytes = mbrtowc(&character, text + offset,
                               length - offset, &state);
        int columns;
        if (bytes == (size_t)-1 || bytes == (size_t)-2 || bytes == 0U) {
            width++;
            offset++;
            (void)memset(&state, 0, sizeof(state));
            continue;
        }
        columns = wcwidth(character);
        width += columns < 0 ? 1U : (size_t)columns;
        offset += bytes;
    }
    return width;
}

static size_t resource_record_path(const gsh_builtin_io *io,
                                   const file_record *record,
                                   char path[GSH_RESOURCE_PROTOCOL_PATH_CAP])
{
    char working_directory[PATH_MAX];
    const char *base;
    const gsh_builtin_resource_sink *resources;
    int length;
    if (io == NULL || io->resources == NULL || record == NULL || path == NULL)
        return 0U;
    resources = io->resources;
    base = resources->base_directory;
    if (record->name[0] == '/') {
        length = snprintf(path, GSH_RESOURCE_PROTOCOL_PATH_CAP, "%s",
                          record->name);
    } else if (base != NULL && base[0] == '/') {
        length = snprintf(path, GSH_RESOURCE_PROTOCOL_PATH_CAP, "%s%s%s",
                          base, strcmp(base, "/") == 0 ? "" : "/",
                          record->name);
    } else if (getcwd(working_directory, sizeof(working_directory)) == NULL) {
        return 0U;
    } else if (base == NULL || base[0] == '\0' || strcmp(base, ".") == 0) {
        length = snprintf(path, GSH_RESOURCE_PROTOCOL_PATH_CAP, "%s/%s",
                          working_directory, record->name);
    } else {
        length = snprintf(path, GSH_RESOURCE_PROTOCOL_PATH_CAP, "%s/%s/%s",
                          working_directory, base, record->name);
    }
    return length > 0 && length < GSH_RESOURCE_PROTOCOL_PATH_CAP
               ? (size_t)length : 0U;
}

static void send_resource_record(const gsh_builtin_io *io,
                                 const file_record *record,
                                 const char *label, size_t label_length)
{
    char message[sizeof(gsh_resource_record_header) +
                 GSH_RESOURCE_PROTOCOL_PATH_CAP +
                 GSH_RESOURCE_PROTOCOL_LABEL_CAP];
    gsh_resource_record_header header;
    const gsh_builtin_resource_sink *resources;
    char path[GSH_RESOURCE_PROTOCOL_PATH_CAP];
    gsh_resource_type type;
    size_t path_length;
    size_t label_columns;

    if (io == NULL || io->resources == NULL || record == NULL ||
        label == NULL ||
        label_length == 0U ||
        label_length >= GSH_RESOURCE_PROTOCOL_LABEL_CAP ||
        !isatty(STDOUT_FILENO)) return;
    resources = io->resources;
    label_columns = file_text_width(label, label_length);
    if (label_columns == 0U) label_columns = 1U;
    if (resources->descriptor < 0 || resources->row > UINT32_MAX ||
        resources->column > UINT32_MAX - label_length ||
        resources->visual_column > UINT32_MAX - label_columns) return;
    type = resource_type_for_mode(record->status.st_mode);
    path_length = resource_record_path(io, record, path);
    if (type == GSH_RESOURCE_UNKNOWN || path_length == 0U) return;
    (void)memset(&header, 0, sizeof(header));
    header.version = GSH_RESOURCE_PROTOCOL_VERSION;
    header.size = (uint32_t)(sizeof(header) + path_length + label_length);
    header.row = (uint32_t)resources->row;
    header.byte_begin = (uint32_t)resources->column;
    header.byte_end = (uint32_t)(resources->column + label_length);
    header.column_begin = (uint32_t)resources->visual_column;
    header.column_end = (uint32_t)(resources->visual_column + label_columns);
    header.type = (uint32_t)type;
    header.flags = resources->navigable_root
                       ? GSH_RESOURCE_PROTOCOL_NAVIGABLE : 0U;
    header.path_length = (uint32_t)path_length;
    header.label_length = (uint32_t)label_length;
    (void)memcpy(message, &header, sizeof(header));
    (void)memcpy(message + sizeof(header), path, path_length);
    (void)memcpy(message + sizeof(header) + path_length, label, label_length);
    (void)write(resources->descriptor, message, header.size);
}

static resource_scope enter_resource_scope(const gsh_builtin_io *io,
                                           const char *base,
                                           bool navigable_root)
{
    resource_scope scope = {0};
    gsh_builtin_resource_sink *sink;
    if (io == NULL || io->resources == NULL) return scope;
    sink = io->resources;
    scope.sink = sink;
    scope.base_directory = sink->base_directory;
    scope.navigable_root = sink->navigable_root;
    sink->base_directory = base;
    sink->navigable_root = navigable_root;
    return scope;
}

static void leave_resource_scope(resource_scope *scope)
{
    gsh_builtin_resource_sink *sink;
    if (scope == NULL || scope->sink == NULL) return;
    sink = scope->sink;
    sink->base_directory = scope->base_directory;
    sink->navigable_root = scope->navigable_root;
}

static int emit_name(const gsh_builtin_io *io, const file_record *record,
                     const ls_options *options)
{
    char name[PATH_MAX];
    char suffix[2] = {'\0', '\0'};
    size_t length;

    if (io == NULL || record == NULL || options == NULL) return 1;
    length = printable_name(record->name, options->quote_nonprintable, name);
    suffix[0] = record->metadata
                    ? type_indicator(record->status.st_mode, options) : '\0';
    send_resource_record(io, record, name, length);
    if (gsh_builtin_output(io, STDOUT_FILENO, name, length) != 0) return 1;
    return suffix[0] == '\0' ? 0 :
           gsh_builtin_output(io, STDOUT_FILENO, suffix, 1U);
}

static int emit_long_record(const gsh_builtin_io *io, int directory_fd,
                            const file_record *record,
                            const ls_options *options)
{
    char mode[11];
    char owner[32];
    char group[32];
    char date[64];
    char link[PATH_MAX];
    char safe_link[PATH_MAX];
    ssize_t link_length = -1;

    if (io == NULL || record == NULL || options == NULL) return 1;
    if (!record->metadata) {
        if (emit_text(io, STDOUT_FILENO, "?????????? ? ? ? ? ? ") != 0 ||
            emit_name(io, record, options) != 0) return 1;
        return emit_text(io, STDOUT_FILENO, "\n");
    }
    mode_text(record->status.st_mode, mode);
    timestamp_text(record, options, date);
    if (options->inode && emit_format(io, STDOUT_FILENO, "%ju ",
            (uintmax_t)record->status.st_ino) != 0) return 1;
    if (options->blocks && emit_format(io, STDOUT_FILENO, "%ju ",
            display_blocks(&record->status, options)) != 0) return 1;
    if (emit_format(io, STDOUT_FILENO, "%s %ju", mode,
                    (uintmax_t)record->status.st_nlink) != 0) return 1;
    if (!options->omit_owner && emit_format(io, STDOUT_FILENO, " %s",
            owner_text(record->status.st_uid, options->numeric, owner)) != 0) return 1;
    if (!options->omit_group && emit_format(io, STDOUT_FILENO, " %s",
            group_text(record->status.st_gid, options->numeric, group)) != 0) return 1;
    if (S_ISCHR(record->status.st_mode) || S_ISBLK(record->status.st_mode)) {
        if (emit_format(io, STDOUT_FILENO, " %ju,%ju %s ",
                (uintmax_t)major(record->status.st_rdev),
                (uintmax_t)minor(record->status.st_rdev), date) != 0) return 1;
    } else if (emit_format(io, STDOUT_FILENO, " %jd %s ",
               (intmax_t)record->status.st_size, date) != 0) return 1;
    if (emit_name(io, record, options) != 0) return 1;
    if (S_ISLNK(record->status.st_mode)) {
        struct stat target;
        int base = directory_fd >= 0 ? directory_fd : AT_FDCWD;
        link_length = readlinkat(base, record->name, link,
                                 sizeof(link) - 1U);
        if (link_length >= 0) {
            char indicator = '\0';
            link[(size_t)link_length] = '\0';
            (void)printable_name(link, options->quote_nonprintable, safe_link);
            if (emit_format(io, STDOUT_FILENO, " -> %s", safe_link) != 0)
                return 1;
            if (fstatat(base, record->name, &target, 0) == 0)
                indicator = type_indicator(target.st_mode, options);
            if (indicator != '\0' &&
                gsh_builtin_output(io, STDOUT_FILENO, &indicator, 1U) != 0)
                return 1;
        }
    }
    return emit_text(io, STDOUT_FILENO, "\n");
}

static size_t terminal_columns(void)
{
    const char *environment = getenv("COLUMNS");
    unsigned long parsed = 0U;
    size_t index;

    if (environment != NULL && environment[0] != '\0') {
        for (index = 0U; environment[index] >= '0' &&
                         environment[index] <= '9'; index++) {
            if (parsed > 10000U) { parsed = 0U; break; }
            parsed = parsed * 10U + (unsigned long)(environment[index] - '0');
        }
        if (environment[index] == '\0' && parsed > 0U) return (size_t)parsed;
    }
    { struct winsize size; if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 &&
          size.ws_col != 0U) return size.ws_col; }
    return 80U;
}

static size_t record_name_width(const file_record *record,
                                const ls_options *options)
{
    char name[PATH_MAX];
    size_t length;
    size_t width;
    uintmax_t value;

    if (record == NULL || options == NULL) return 0U;
    length = printable_name(record->name, options->quote_nonprintable, name);
    width = file_text_width(name, length);
    if (record->metadata && type_indicator(record->status.st_mode, options) != '\0') width++;
    value = (uintmax_t)record->status.st_ino;
    if (options->inode) {
        do { width++; value /= 10U; } while (value != 0U);
        width++;
    }
    value = display_blocks(&record->status, options);
    if (options->blocks) {
        do { width++; value /= 10U; } while (value != 0U);
        width++;
    }
    return width;
}

static int emit_prefix(const gsh_builtin_io *io, const file_record *record,
                       const ls_options *options)
{
    if (io == NULL || record == NULL || options == NULL) return 1;
    if (options->inode && emit_format(io, STDOUT_FILENO, "%ju ",
            (uintmax_t)record->status.st_ino) != 0) return 1;
    if (options->blocks && emit_format(io, STDOUT_FILENO, "%ju ",
            display_blocks(&record->status, options)) != 0) return 1;
    return 0;
}

static int emit_simple_catalog(const gsh_builtin_io *io,
                               const file_catalog *catalog,
                               const ls_options *options)
{
    size_t index;

    if (io == NULL || catalog == NULL || options == NULL) return 1;
    for (index = 0U; index < catalog->count; index++) {
        file_record record;
        if (catalog_get(catalog, index, &record) == -1 ||
            emit_prefix(io, &record, options) != 0 ||
            emit_name(io, &record, options) != 0 ||
            emit_text(io, STDOUT_FILENO, "\n") != 0) return 1;
    }
    return 0;
}

static int emit_comma_catalog(const gsh_builtin_io *io,
                              const file_catalog *catalog,
                              const ls_options *options)
{
    size_t line = 0U;
    size_t index;

    if (io == NULL || catalog == NULL || options == NULL) return 1;
    for (index = 0U; index < catalog->count; index++) {
        file_record record;
        size_t width;
        if (catalog_get(catalog, index, &record) == -1) return 1;
        width = record_name_width(&record, options);
        if (index != 0U) {
            const char *separator = line + 2U + width > terminal_columns()
                                        ? ",\n" : ", ";
            if (emit_text(io, STDOUT_FILENO, separator) != 0) return 1;
            line = separator[1] == '\n' ? 0U : line + 2U;
        }
        if (emit_prefix(io, &record, options) != 0 ||
            emit_name(io, &record, options) != 0) return 1;
        line += width;
    }
    return catalog->count == 0U ? 0 : emit_text(io, STDOUT_FILENO, "\n");
}

static int emit_columns(const gsh_builtin_io *io,
                        const file_catalog *catalog,
                        const ls_options *options)
{
    size_t widest = 1U;
    size_t columns;
    size_t rows;
    size_t row;
    size_t index;

    if (io == NULL || catalog == NULL || options == NULL) return 1;
    for (index = 0U; index < catalog->count; index++) {
        file_record record;
        if (catalog_get(catalog, index, &record) == -1) return 1;
        if (record_name_width(&record, options) > widest) widest = record_name_width(&record, options);
    }
    widest += 2U;
    columns = terminal_columns() / widest;
    if (columns == 0U) columns = 1U;
    rows = (catalog->count + columns - 1U) / columns;
    for (row = 0U; row < rows; row++) {
        size_t column;
        for (column = 0U; column < columns; column++) {
            file_record record;
            size_t slot = options->format == LS_FORMAT_ACROSS
                              ? row * columns + column : column * rows + row;
            size_t width;
            if (slot >= catalog->count) continue;
            if (catalog_get(catalog, slot, &record) == -1 ||
                emit_prefix(io, &record, options) != 0 ||
                emit_name(io, &record, options) != 0) return 1;
            width = record_name_width(&record, options);
            if (column + 1U < columns &&
                ((options->format == LS_FORMAT_ACROSS &&
                  slot + 1U < catalog->count) ||
                 (options->format != LS_FORMAT_ACROSS &&
                  slot + rows < catalog->count)) &&
                emit_format(io, STDOUT_FILENO, "%*s",
                            (int)(widest - width), "") != 0) return 1;
        }
        if (emit_text(io, STDOUT_FILENO, "\n") != 0) return 1;
    }
    return 0;
}

static int emit_catalog(const gsh_builtin_io *io, int directory_fd,
                        const file_catalog *catalog,
                        const ls_options *options)
{
    size_t index;

    if (io == NULL || catalog == NULL || options == NULL) return 1;
    if (options->format == LS_FORMAT_LONG) {
        for (index = 0U; index < catalog->count; index++) {
            file_record record;
            if (catalog_get(catalog, index, &record) == -1 ||
                emit_long_record(io, directory_fd, &record, options) != 0) return 1;
        }
        return 0;
    }
    if (options->format == LS_FORMAT_COLUMNS ||
        options->format == LS_FORMAT_ACROSS) return emit_columns(io, catalog, options);
    if (options->format == LS_FORMAT_COMMAS)
        return emit_comma_catalog(io, catalog, options);
    return emit_simple_catalog(io, catalog, options);
}

static int recursion_ancestor(const char *parent,
                              const struct stat *target)
{
    int descriptor;
    size_t depth;
    if (parent == NULL || target == NULL) return -1;
    descriptor = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) return -1;
    for (depth = 0U; depth < GSH_FILE_RECURSION_CAP; depth++) {
        struct stat current;
        struct stat upper;
        int next;
        if (fstat(descriptor, &current) == -1) {
            (void)close(descriptor);
            return -1;
        }
        if (current.st_dev == target->st_dev &&
            current.st_ino == target->st_ino) {
            (void)close(descriptor);
            return 1;
        }
        next = openat(descriptor, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (next < 0 || fstat(next, &upper) == -1) {
            int saved = errno;
            if (next >= 0) (void)close(next);
            (void)close(descriptor);
            errno = saved;
            return -1;
        }
        if (current.st_dev == upper.st_dev && current.st_ino == upper.st_ino) {
            (void)close(next);
            (void)close(descriptor);
            return 0;
        }
        (void)close(descriptor);
        descriptor = next;
    }
    (void)close(descriptor);
    errno = EOVERFLOW;
    return -1;
}

static int queue_recursion(file_workspace *space, const char *parent,
                           const file_record *record)
{
    recursion_item *item;
    int length;

    if (space == NULL || parent == NULL || record == NULL ||
        space->pending_count >= GSH_FILE_RECURSION_CAP) {
        errno = EOVERFLOW;
        return -1;
    }
    if (!record->metadata || !S_ISDIR(record->status.st_mode) ||
        strcmp(record->name, ".") == 0 || strcmp(record->name, "..") == 0)
        return 0;
    {
        int ancestor = recursion_ancestor(parent, &record->status);
        if (ancestor < 0) return -1;
        if (ancestor != 0) {
            errno = ELOOP;
            return -1;
        }
    }
    item = &space->pending[space->pending_count];
    length = snprintf(item->path, sizeof(item->path), "%s%s%s", parent,
                      strcmp(parent, "/") == 0 ? "" : "/", record->name);
    if (length < 0 || (size_t)length >= sizeof(item->path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    space->pending_count++;
    return 0;
}

static int queue_catalog_directories(file_workspace *space,
                                     const file_catalog *catalog,
                                     const char *parent)
{
    size_t index;
    if (space == NULL || catalog == NULL || parent == NULL) return -1;
    for (index = catalog->count; index > 0U; index--) {
        file_record record;
        if (catalog_get(catalog, index - 1U, &record) == -1 ||
            queue_recursion(space, parent, &record) == -1) return -1;
    }
    return 0;
}

static int list_directory(const gsh_builtin_io *io, const char *path,
                          const ls_options *options, bool heading,
                          bool navigable_root, file_workspace *space)
{
    int descriptor;
    int result = 0;
    resource_scope resources;

    if (io == NULL || path == NULL || options == NULL || space == NULL) return 1;
    descriptor = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor == -1) return file_error(io, "ls", path, errno);
    resources = enter_resource_scope(io, path, navigable_root);
    catalog_reset(&space->catalog);
    if (enumerate_directory(dup(descriptor), options, &space->catalog) == -1 ||
        catalog_sort(&space->catalog, options) == -1) {
        result = file_error(io, "ls", path, errno);
    } else {
        if (heading) {
            char display_path[PATH_MAX];
            (void)printable_name(path, options->quote_nonprintable,
                                 display_path);
            if (emit_format(io, STDOUT_FILENO, "%s:\n", display_path) != 0)
                result = 1;
        }
        if (options->format == LS_FORMAT_LONG || options->blocks) {
            uintmax_t blocks = 0U;
            size_t index;
            for (index = 0U; index < space->catalog.count; index++) {
                file_record record;
                if (catalog_get(&space->catalog, index, &record) == -1) { result = 1; break; }
                if (record.metadata && record.status.st_blocks > 0)
                    blocks += (uintmax_t)record.status.st_blocks;
            }
            if (options->kib_blocks) blocks = (blocks + 1U) / 2U;
            if (result == 0 && emit_format(io, STDOUT_FILENO, "total %ju\n", blocks) != 0) result = 1;
        }
        if (result == 0) result = emit_catalog(io, descriptor, &space->catalog, options);
        if (result == 0 && options->recursive &&
            queue_catalog_directories(space, &space->catalog, path) == -1) {
            result = file_error(io, "ls", path, errno);
        }
    }
    catalog_reset(&space->catalog);
    (void)close(descriptor);
    leave_resource_scope(&resources);
    return result;
}

static int stat_operand(const char *path, const ls_options *options,
                        file_record *record)
{
    size_t length;

    if (path == NULL || options == NULL || record == NULL) return -1;
    (void)memset(record, 0, sizeof(*record));
    length = strlen(path);
    if (length >= sizeof(record->name)) { errno = ENAMETOOLONG; return -1; }
    (void)memcpy(record->name, path, length + 1U);
    record->metadata = fstatat(AT_FDCWD, path, &record->status,
                               AT_SYMLINK_NOFOLLOW) == 0;
    if (record->metadata && S_ISLNK(record->status.st_mode) &&
        (options->dereference_all || options->dereference_operands)) {
        record->metadata = fstatat(AT_FDCWD, path, &record->status, 0) == 0;
    } else if (record->metadata && S_ISLNK(record->status.st_mode) &&
               !options->directory && !options->classify &&
               options->format != LS_FORMAT_LONG) {
        struct stat target;
        if (fstatat(AT_FDCWD, path, &target, 0) == 0 &&
            S_ISDIR(target.st_mode)) record->status = target;
    }
    return record->metadata ? 0 : -1;
}

static void sort_operand_records(file_record records[GSH_FILE_OPERAND_CAP],
                                 size_t count,
                                 const ls_options *options)
{
    size_t index;
    if (records == NULL || options == NULL) return;
    for (index = 1U; index < count; index++) {
        file_record item = records[index];
        size_t position = index;
        while (position > 0U && record_compare(
                   &item, &records[position - 1U], options) < 0) {
            records[position] = records[position - 1U];
            position--;
        }
        records[position] = item;
    }
}

static int run_ls(size_t argc, char *const argv[], const gsh_builtin_io *io)
{
    file_workspace space;
    file_record records[GSH_FILE_OPERAND_CAP];
    ls_options options;
    size_t first;
    size_t argument;
    size_t operands;
    size_t record_count = 0U;
    bool wrote_group = false;
    int result = 0;

    if (argc == 0U || argv == NULL || io == NULL) return 1;
    (void)memset(&space, 0, sizeof(space));
    space.catalog.spool[0] = -1;
    space.catalog.spool[1] = -1;
    if (parse_ls_options(argc, argv, &options, &first) == -1) {
        return gsh_builtin_error(io, "ls", "invalid option");
    }
    operands = argc - first;
    if (operands > GSH_FILE_OPERAND_CAP) {
        return gsh_builtin_error(io, "ls", "too many operands");
    }
    for (argument = 0U; argument < (operands == 0U ? 1U : operands);
         argument++) {
        file_record record;
        const char *path = operands == 0U ? "." : argv[first + argument];
        if (stat_operand(path, &options, &record) == -1) {
            result = file_error(io, "ls", path, errno);
            continue;
        }
        record.sequence = argument;
        records[record_count++] = record;
    }
    sort_operand_records(records, record_count, &options);
    for (argument = 0U; argument < record_count; argument++) {
        if ((options.directory || !S_ISDIR(records[argument].status.st_mode)) &&
            catalog_add(&space.catalog, &records[argument]) == -1) result = 1;
    }
    if (space.catalog.count != 0U) {
        if (emit_catalog(io, -1, &space.catalog, &options) != 0) result = 1;
        wrote_group = true;
    }
    catalog_reset(&space.catalog);
    for (argument = 0U; argument < record_count; argument++) {
        if (options.directory || !S_ISDIR(records[argument].status.st_mode))
            continue;
        space.pending_count = 0U;
        if (wrote_group && emit_text(io, STDOUT_FILENO, "\n") != 0) result = 1;
        if (list_directory(io, records[argument].name, &options,
                           operands > 1U || options.recursive,
                           operands <= 1U && !options.recursive,
                           &space) != 0) result = 1;
        wrote_group = true;
        while (space.pending_count > 0U) {
            recursion_item item = space.pending[--space.pending_count];
            if (emit_text(io, STDOUT_FILENO, "\n") != 0 ||
                list_directory(io, item.path, &options, true, false,
                               &space) != 0)
                result = 1;
        }
    }
    catalog_reset(&space.catalog);
    return result;
}

static const char *ll_type(mode_t mode)
{
    if (S_ISDIR(mode)) return "DIR";
    if (S_ISLNK(mode)) return "LINK";
    if (S_ISREG(mode)) return "FILE";
    if (S_ISFIFO(mode)) return "FIFO";
    if (S_ISSOCK(mode)) return "SOCKET";
    if (S_ISCHR(mode) || S_ISBLK(mode)) return "DEVICE";
    return "OTHER";
}

static void human_size(off_t size, char text[16])
{
    static const char units[] = "BKMGTPE";
    double value = size < 0 ? 0.0 : (double)size;
    size_t unit = 0U;

    if (text == NULL) return;
    while (value >= 1024.0 && unit + 1U < sizeof(units) - 1U) {
        value /= 1024.0;
        unit++;
    }
    if (unit == 0U) (void)snprintf(text, 16U, "%.0fB", value);
    else if (value < 10.0) (void)snprintf(text, 16U, "%.1f%c", value, units[unit]);
    else (void)snprintf(text, 16U, "%.0f%c", value, units[unit]);
}

static int emit_ll_name(const gsh_builtin_io *io,
                        const file_record *record, const char *override,
                        size_t width)
{
    char name[PATH_MAX];
    char field[GSH_RESOURCE_PROTOCOL_LABEL_CAP];
    size_t length;
    size_t columns;
    size_t padding;
    if (io == NULL || record == NULL) return 1;
    if (override == NULL) length = printable_name(record->name, true, name);
    else {
        length = strlen(override);
        if (length >= sizeof(name)) return 1;
        (void)memcpy(name, override, length + 1U);
    }
    columns = file_text_width(name, length);
    padding = width > columns ? width - columns : 0U;
    if (length + padding < sizeof(field)) {
        (void)memcpy(field, name, length);
        (void)memset(field + length, ' ', padding);
        send_resource_record(io, record, field, length + padding);
        if (gsh_builtin_output(io, STDOUT_FILENO, field,
                               length + padding) != 0) return 1;
    } else {
        send_resource_record(io, record, name, length);
        if (gsh_builtin_output(io, STDOUT_FILENO, name, length) != 0 ||
            emit_format(io, STDOUT_FILENO, "%*s", (int)padding, "") != 0)
            return 1;
    }
    return emit_text(io, STDOUT_FILENO, "  ");
}

static int emit_ll_record_label(const gsh_builtin_io *io,
                                const file_record *record,
                                const char *label, size_t name_width,
                                size_t columns)
{
    char mode[11];
    char owner[32];
    char group[32];
    char date[64];
    char size[16];
    const char *owner_value;
    const char *group_value;
    bool show_type = columns >= 68U;
    bool show_owner = columns >= 78U;
    bool show_group = columns >= 88U;
    bool show_links = columns >= 100U;

    if (io == NULL || record == NULL) return 1;
    if (!record->metadata) {
        if (emit_ll_name(io, record, label, name_width) != 0) return 1;
        return emit_text(io, STDOUT_FILENO, "?\n");
    }
    mode_text(record->status.st_mode, mode);
    human_size(record->status.st_size, size);
    { ls_options time_options; ls_defaults(&time_options); timestamp_text(record, &time_options, date); }
    owner_value = owner_text(record->status.st_uid, false, owner);
    group_value = group_text(record->status.st_gid, false, group);
    if (emit_ll_name(io, record, label, name_width) != 0) return 1;
    if (show_type && emit_format(io, STDOUT_FILENO, "%-6s ", ll_type(record->status.st_mode)) != 0) return 1;
    if (emit_format(io, STDOUT_FILENO, "%s ", mode) != 0) return 1;
    if (show_links && emit_format(io, STDOUT_FILENO, "%4ju ", (uintmax_t)record->status.st_nlink) != 0) return 1;
    if (show_owner && emit_format(io, STDOUT_FILENO, "%-10.10s ", owner_value) != 0) return 1;
    if (show_group && emit_format(io, STDOUT_FILENO, "%-10.10s ", group_value) != 0) return 1;
    return emit_format(io, STDOUT_FILENO, "%8s  %s\n", size, date);
}

static int emit_ll_record(const gsh_builtin_io *io,
                          const file_record *record, size_t name_width,
                          size_t columns)
{
    return emit_ll_record_label(io, record, NULL, name_width, columns);
}

static bool ll_back_record(const gsh_builtin_io *io, const char *path,
                           file_record *record)
{
    char directory[PATH_MAX];
    char *slash;
    if (io == NULL || io->resources == NULL || path == NULL ||
        record == NULL || !isatty(STDOUT_FILENO) ||
        realpath(path, directory) == NULL || strcmp(directory, "/") == 0)
        return false;
    slash = strrchr(directory, '/');
    if (slash == NULL) return false;
    if (slash == directory) slash[1] = '\0';
    else *slash = '\0';
    (void)memset(record, 0, sizeof(*record));
    if (strlen(directory) >= sizeof(record->name) ||
        stat(directory, &record->status) == -1 ||
        !S_ISDIR(record->status.st_mode)) return false;
    (void)memcpy(record->name, directory, strlen(directory) + 1U);
    record->metadata = true;
    return true;
}

static int ll_measure_names(const file_catalog *catalog, size_t *name_width)
{
    size_t index;
    if (catalog == NULL || name_width == NULL) return -1;
    for (index = 0U; index < catalog->count; index++) {
        file_record record;
        char name[PATH_MAX];
        size_t length;
        size_t width;
        if (catalog_get(catalog, index, &record) == -1) return -1;
        length = printable_name(record.name, true, name);
        width = file_text_width(name, length);
        if (width > *name_width) *name_width = width;
    }
    return 0;
}

static int ll_emit_catalog(const gsh_builtin_io *io,
                           const file_catalog *catalog,
                           size_t name_width, size_t columns)
{
    size_t index;
    if (io == NULL || catalog == NULL) return 1;
    for (index = 0U; index < catalog->count; index++) {
        file_record record;
        if (catalog_get(catalog, index, &record) == -1 ||
            emit_ll_record(io, &record, name_width, columns) != 0) return 1;
    }
    return 0;
}

static int run_ll(size_t argc, char *const argv[], const gsh_builtin_io *io)
{
    file_workspace space;
    ls_options options;
    const char *path = ".";
    size_t argument = 1U;
    int descriptor;
    size_t name_width = 4U;
    size_t columns;
    resource_scope resources;
    file_record back;
    bool have_back;
    int result = 0;

    if (argc == 0U || argv == NULL || io == NULL) return 1;
    (void)memset(&space, 0, sizeof(space));
    space.catalog.spool[0] = -1;
    space.catalog.spool[1] = -1;
    ls_defaults(&options);
    options.sort = LS_SORT_NATURAL;
    options.format = LS_FORMAT_LONG;
    if (argument < argc && strcmp(argv[argument], "-a") == 0) { options.all = true; argument++; }
    if (argument < argc && strcmp(argv[argument], "--") == 0) argument++;
    if (argument < argc) path = argv[argument++];
    if (argument != argc) return gsh_builtin_error(io, "ll", "usage: ll [-a] [--] [PATH]");
    descriptor = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor == -1) return file_error(io, "ll", path, errno);
    resources = enter_resource_scope(io, path, true);
    have_back = ll_back_record(io, path, &back);
    catalog_reset(&space.catalog);
    if (enumerate_directory(dup(descriptor), &options, &space.catalog) == -1 ||
        catalog_sort(&space.catalog, &options) == -1) {
        result = file_error(io, "ll", path, errno);
    }
    if (result == 0 && ll_measure_names(&space.catalog, &name_width) == -1)
        result = 1;
    columns = terminal_columns();
    if (name_width > columns / 2U) name_width = columns / 2U;
    if (result == 0 && have_back &&
        emit_ll_record_label(io, &back, "<-", name_width, columns) != 0)
        result = 1;
    if (result == 0 &&
        ll_emit_catalog(io, &space.catalog, name_width, columns) != 0)
        result = 1;
    catalog_reset(&space.catalog);
    (void)close(descriptor);
    leave_resource_scope(&resources);
    return result;
}

static int copy_file(const char *path, const gsh_builtin_io *io)
{
    char buffer[16384];
    int descriptor;
    size_t turns;

    if (path == NULL || io == NULL) return 1;
    descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor == -1) return file_error(io, "view", path, errno);
    for (turns = 0U; turns < GSH_VIEW_INDEX_CAP; turns++) {
        ssize_t count = read(descriptor, buffer, sizeof(buffer));
        if (count > 0 && gsh_builtin_output(io, STDOUT_FILENO, buffer,
                                            (size_t)count) != 0) { (void)close(descriptor); return 1; }
        if (count == 0) { (void)close(descriptor); return 0; }
        if (count == -1 && errno == EINTR) { turns--; continue; }
        if (count == -1) { int saved = errno; (void)close(descriptor); return file_error(io, "view", path, saved); }
    }
    (void)close(descriptor);
    return file_error(io, "view", path, EFBIG);
}

static int parse_view_location(const char *argument, char path[PATH_MAX],
                               size_t *line, size_t *column)
{
    size_t length;
    size_t split;
    unsigned long values[2] = {0U, 0U};
    size_t found = 0U;

    if (argument == NULL || path == NULL || line == NULL || column == NULL) return -1;
    length = strlen(argument);
    if (length >= PATH_MAX) { errno = ENAMETOOLONG; return -1; }
    (void)memcpy(path, argument, length + 1U);
    if (access(path, F_OK) == 0) { *line = 1U; *column = 1U; return 0; }
    split = length;
    while (split > 0U && found < 2U) {
        size_t end = split;
        size_t begin;
        while (split > 0U && path[split - 1U] >= '0' && path[split - 1U] <= '9') split--;
        begin = split;
        if (begin == end || begin == 0U || path[begin - 1U] != ':') break;
        { size_t index; unsigned long value = 0U;
          for (index = begin; index < end; index++) {
              if (value > (ULONG_MAX - (unsigned long)(path[index] - '0')) / 10U) return -1;
              value = value * 10U + (unsigned long)(path[index] - '0');
          }
          values[found++] = value; }
        split = begin - 1U;
    }
    if (found == 0U || split == 0U) { errno = ENOENT; return -1; }
    path[split] = '\0';
    *line = found == 2U ? (size_t)values[1] : (size_t)values[0];
    *column = found == 2U ? (size_t)values[0] : 1U;
    if (*line == 0U) *line = 1U;
    if (*column == 0U) *column = 1U;
    return 0;
}

static int run_view(size_t argc, char *const argv[], const gsh_builtin_io *io)
{
    size_t argument = 1U;
    char path[PATH_MAX];
    size_t line;
    size_t column;
    struct stat status;

    if (argc == 0U || argv == NULL || io == NULL) return 1;
    if (argument < argc && strcmp(argv[argument], "--") == 0) argument++;
    if (argument + 1U != argc) return gsh_builtin_error(io, "view", "usage: view [--] FILE");
    if (parse_view_location(argv[argument], path, &line, &column) == -1) return file_error(io, "view", argv[argument], errno);
    if (stat(path, &status) == -1) return file_error(io, "view", path, errno);
    if (S_ISDIR(status.st_mode)) return gsh_builtin_error(io, "view", "is a directory; use ll");
    if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) {
        return gsh_native_viewer(path, line, column, io);
    }
    return copy_file(path, io);
}

int gsh_builtin_run_files(gsh_file_builtin_kind kind, size_t argc,
                          char *const argv[], const gsh_builtin_io *io)
{
    if (argv == NULL || io == NULL || argc == 0U) return 1;
    if (kind == GSH_FILE_BUILTIN_LS) return run_ls(argc, argv, io);
    if (kind == GSH_FILE_BUILTIN_LL) return run_ll(argc, argv, io);
    if (kind == GSH_FILE_BUILTIN_VIEW) return run_view(argc, argv, io);
    errno = EINVAL;
    return 1;
}
