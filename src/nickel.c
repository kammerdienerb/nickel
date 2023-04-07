/* nickel.c -- an interpreter for Nickel, a tiny LISP
 *
 * This language was created as an instructional aid for teaching a university
 * course on programming languages.
 *
 *                                            Brandon Kammerdiener, April 2023
 */


#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <ctype.h>
#include <stdarg.h>
#include <time.h>

#include "array.c"
#include "hash_table.h"

#define ERROR(fmt, ...)                           \
do {                                              \
    printf("Nickel: error: " fmt, ##__VA_ARGS__); \
    exit(__LINE__);                               \
} while (0)



/*** Define the kinds of syntax nodes our language contains. ***/
enum {
    INVALID,
    PROGRAM,
    LIST,
    INT_ATOM,
    STRING_ATOM,
    NAME_ATOM,
};

typedef struct {
    union {
        array_t     children;
        long long   integer;
        const char *string;
        const char *name;
        void       *_v;
    };
    int kind;
} Node;





/*** Utility functions to make/copy/free/print nodes ***/
static Node make_node(int kind) {
    Node node;

    memset(&node, 0, sizeof(node));

    node.kind = kind;

    if (kind == LIST || kind == PROGRAM) {
        node.children = array_make(Node);
    }

    return node;
}

static Node make_list(void) {
    return make_node(LIST);
}

static Node make_int(long long i) {
    Node node;

    node         = make_node(INT_ATOM);
    node.integer = i;

    return node;
}

static Node make_string(const char *s) {
    Node node;

    node        = make_node(STRING_ATOM);
    node.string = strdup(s);

    return node;
}

static Node make_string_no_dup(const char *s) {
    Node node;

    node        = make_node(STRING_ATOM);
    node.string = s;

    return node;
}

static Node make_name(const char *s) {
    Node node;

    node      = make_node(NAME_ATOM);
    node.name = strdup(s);

    return node;
}

static Node copy_node(Node *node) {
    Node  new;
    Node *it;
    Node  new_child;

    switch (node->kind) {
        case INT_ATOM:
            new = make_int(node->integer);
            break;
        case STRING_ATOM:
            new = make_string(node->string);
            break;
        case NAME_ATOM:
            new = make_name(node->name);
            break;
        case LIST:
            new = make_list();
            array_traverse(node->children, it) {
                new_child = copy_node(it);
                array_push(new.children, new_child);
            }
            break;
    }

    return new;
}

static void free_node(Node *node) {
    Node *it;

    switch (node->kind) {
        case LIST:
            array_traverse(node->children, it) {
                free_node(it);
            }
            array_free(node->children);
            break;
        case INT_ATOM:
            break;
        case STRING_ATOM:
            free((char*)node->string);
            break;
        case NAME_ATOM:
            free((char*)node->name);
            break;
        default:
            break;
    }
}

static void _node_to_string(array_t *chars, Node *node) {
    char  buff[32];
    Node *it;

#define PUSHC(c) { char _c=(c); array_push(*chars, _c); }
#define PUSHS(s) { const char *_s=(s); while (*_s) { PUSHC(*_s++); } }

    switch (node->kind) {
        case PROGRAM:
            array_traverse(node->children, it) {
                _node_to_string(chars, it);
                PUSHC('\n');
            }
            break;
        case LIST:
            PUSHC('['); PUSHC(' ');
            array_traverse(node->children, it) {
                _node_to_string(chars, it);
                PUSHC(' ');
            }
            PUSHC(']');
            break;
        case INT_ATOM:
            snprintf(buff, sizeof(buff), "%lld", node->integer);
            PUSHS(buff);
            break;
        case STRING_ATOM:
            PUSHS(node->string);
            break;
        case NAME_ATOM:
            PUSHS("<name ");
            PUSHS(node->name);
            PUSHC('>');
            break;
        default:
            break;
    }
}

static char *node_to_string(Node *node) {
    array_t chars;

    chars = array_make(char);
    _node_to_string(&chars, node);

    array_zero_term(chars);

    return array_data(chars);
}

static void print_node(Node *node) {
    char *str;

    str = node_to_string(node);
    fwrite(str, 1, strlen(str), stdout);
    fwrite("\n", 1, 1, stdout);
    free(str);
}


/*** Our top-level data: ***/

/* The root node. */
static Node program;

/* Our place in the file we are currently parsing. */
static const char *cursor;

/* Current line number. */
static unsigned line = 1;

/* Set up some things for a hash_table, which we'll use as a symbol table. */
static uint64_t str_hash(const char *s) {
    uint64_t hash = 5381;
    int c;

    while ((c = *s++))
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */

    return hash;
}
static int str_equ(const char *a, const char *b) { return strcmp(a, b) == 0; }
typedef const char *fn_name_t;
use_hash_table(fn_name_t, array_t);

/* Symbol table for user-defined functions */
static hash_table(fn_name_t, array_t) functions;

/* An argument stack. */
static array_t args;



/*** Parsing code. ***/

/* Macro to clean up whitespace and consume comments. */
#define CLEAN()                                              \
do {                                                         \
    while (*cursor && isspace(*cursor)) {                    \
        if (*cursor == '\n') { line += 1; }                  \
        cursor += 1;                                         \
    }                                                        \
    while (*cursor == ';') {                                 \
        while (*cursor && *cursor != '\n')  { cursor += 1; } \
        while (*cursor && isspace(*cursor)) {                \
            if (*cursor == '\n') { line += 1; }              \
            cursor += 1;                                     \
        }                                                    \
    }                                                        \
} while (0)

/* Load a file directly into memory. */
static const char * mmap_file(const char *path) {
    int          fd;
    struct stat  fs;
    void        *p;

    fd = open(path, O_RDONLY);
    if (fd < 0) { return NULL; }

    fstat(fd, &fs);

    p = mmap(NULL, fs.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) { p = NULL; }

    return p;
}

/* A recursive-descent subroutine for parsing nodes and lists of nodes. */
static Node parse_node(void) {
    Node        node;
    long long   i;
    long long   j;
    char       *p;
    char       *new_cursor;
    Node        child;

    node.kind = INVALID;

    CLEAN();

    if (*cursor == 0) {
        /* ignore end of file */
    } else if (isdigit(*cursor) || (*cursor == '-' && isdigit(*(cursor + 1)))) {
        i = strtoll(cursor, &new_cursor, 10);
        if (new_cursor == cursor) {
            ERROR("line %u: bad integer\n", line);
        }
        cursor = new_cursor;

        node.kind    = INT_ATOM;
        node.integer = i;
    } else if (*cursor == '[') {
        cursor += 1;

        CLEAN();
        node = make_node(LIST);

        while (*cursor != ']' && (child = parse_node()).kind != INVALID) {
            array_push(node.children, child);
            CLEAN();
        }

        CLEAN();

        if (*cursor != ']') {
            ERROR("line %u: expected closing ']'\n", line);
        }

        cursor += 1;
    } else if (*cursor == '"') {
        node = make_node(STRING_ATOM);

        cursor += 1;

        i = 0;
        while (cursor[i] && (cursor[i] != '"' || (i > 0 && cursor[i-1] == '\\'))) {
            i += 1;
        }

        node.string = p = malloc(i + 1);
        for (j = 0; j < i; j += 1) {
            if (cursor[j] != '\\') {
                if (j > 0 && cursor[j - 1] == '\\') {
                    switch (cursor[j]) {
                        case 'n':
                            *(p++) = '\n';
                            break;
                        case 'r':
                            *(p++) = '\r';
                            break;
                        case 't':
                            *(p++) = '\t';
                            break;
                        case '0':
                            *(p++) = 0;
                            break;
                        case '"':
                            *(p++) = '"';
                            break;
                        case '\\':
                            *(p++) = '\\';
                            break;
                        default:
                            *(p++) = '\\';
                            *(p++) = cursor[j];
                            break;
                    }
                } else {
                    *(p++) = cursor[j];
                }
            }
        }
        *p = 0;

        cursor += i;

        if (*cursor != '"') {
            ERROR("line %u: expected closing '\"'\n", line);
        }

        cursor += 1;
    } else if (*cursor != ']') {
        node = make_node(NAME_ATOM);

        i = 0;
        while (cursor[i] && !isspace(cursor[i]) && cursor[i] != ']') {
            i += 1;
        }

        node.name = strndup(cursor, i);

        cursor += i;
    } else {
        ERROR("line %u: unexpected character '%c'\n", line, *cursor);
    }

    return node;
}


/***
 *** Interpretation of Nickel nodes. Most of the interesting work is done
 *** here. The code below takes nodes produces new nodes representing the
 *** result of evaluation of the input nodes. We also do things like
 *** dynamic type checking and evaluation of special-form nodes,
 *** including function definition.
 ***/

/* Convenience macro to get a child node from a list. */
#define CHILD(_children, _idx) \
    ((Node*)array_item((_children), (_idx)))

/* Type check the application of a function.
 * `arity` indicates how many arguments are expected and the following
 * varargs should indicate the node kind that is expected for each argument.
 * For example, the function `+` expects two INT_ATOM arguments, so is checked
 *     `check(&evaluated_nodes, 2, INT_ATOM, INT_ATOM);`
 * If an argument can be of any type, -1 should be passed as the kind.
 */
static void check(array_t *evaluated_nodes, int arity, ...) {
    int      n_args;
    va_list  args;
    int      i;
    Node    *arg;
    int      expected;

    n_args = array_len(*evaluated_nodes) - 1;
    if (n_args != arity) {
        ERROR("in application of function '%s': expected %d arguments, but got %d\n",
              CHILD(*evaluated_nodes, 0)->name, arity, n_args);
    }

    va_start(args, arity);

    for (i = 1; i <= arity; i += 1) {
        arg      = CHILD(*evaluated_nodes, i);
        expected = va_arg(args, int);
        if (expected != -1 && arg->kind != expected) {
            ERROR("in application of function '%s': incorrect type (argument %d)\n",
                  CHILD(*evaluated_nodes, 0)->name, i);
        }
    }

    va_end(args);
}

static Node interpret(Node *node);

/* Interpret the `if` special form. It is important that only one of the
 * result expressions is evaluated, unlike normal function application in
 * which all argument expressions are fully evaluated first. */
static Node interpret_if(Node *node) {
    Node      *it;
    Node       cond;
    long long  val;

    if (array_len(node->children) < 3) {
        ERROR("if expects a condition and at least a true expression\n");
    }

    it   = CHILD(node->children, 1);
    cond = interpret(it);
    val  = cond.integer;

    if (cond.kind != INT_ATOM) {
        ERROR("if condition must evaluate to an integer\n");
    }

    free_node(&cond);

    if (val) {
        return interpret(CHILD(node->children, 2));
    } else if (array_len(node->children) >= 4) {
        return interpret(CHILD(node->children, 3));
    }

    return make_int(0);
}

/* Interpret the `define` special form, which defines a new function. This
   function is pretty short and simply copies the function (as a tree) into
   our symbol table, keyed on the name. Replace the function if it is already
   in the table. */
static Node interpret_define(Node *node) {
    Node       name;
    fn_name_t *lookup;
    fn_name_t  key;
    array_t    existing;
    Node      *it;
    array_t    expressions;
    Node       expr;

    if (array_len(node->children) < 3) {
        ERROR("define expects a name and at least one expression\n");
    }

    name = copy_node(CHILD(node->children, 1));

    lookup = hash_table_get_key(functions, name.name);
    if (lookup != NULL) {
        key      = *lookup;
        existing = *hash_table_get_val(functions, key);
        hash_table_delete(functions, key);
    }

    expressions = array_make(Node);

    array_traverse_from(node->children, it, 2) {
        expr = copy_node(it);
        array_push(expressions, expr);
    }

    hash_table_insert(functions, strdup(name.name), expressions);

    if (lookup != NULL) {
        array_traverse(existing, it) {
            free_node(it);
        }
        array_free(existing);
        free((char*)key);
    }

    return name;
}

/* Implementation of the fmt functions that uses printf's formatting. */
static const char *do_fmt(array_t nodes) {
    array_t     chars;
    const char *fmt;
    int         node_idx;
    char        last;
    char        c;
    char        buff[64];
    char       *buffp;
    int         var_width;
    char       *node_str;
    char       *str;

    chars = array_make(char);
    array_zero_term(chars);

    fmt = CHILD(nodes, 1)->string;

    node_idx = 2;

    last = 0;
    while ((c = *fmt)) {
        if (c == '{') {
            if (last == '\\') {
                array_pop(chars);
                array_push(chars, c);
            } else {
                fmt += 1;

                buff[0]   = '%';
                buffp     = buff + 1;
                var_width = 0;
                while ((c = *fmt) && c != '}') {
                    if (c == '*') { var_width = 1; }
                    *(buffp++) = c;
                    fmt += 1;
                }
                *buffp = 0;

                if (*fmt == 0) { break; }

                if (array_len(nodes) <= node_idx + var_width) {
                    ERROR("format missing argument\n");
                }

                if (!isalpha(buff[strlen(buff) - 1])) {
                    *buffp = 's';
                    buffp += 1;
                    *buffp = 0;

                    if (var_width) {
                        node_str = node_to_string(CHILD(nodes, node_idx + 1));
                        asprintf(&str, buff, CHILD(nodes, node_idx)->integer, node_str);
                        node_idx += 2;
                    } else {
                        node_str = node_to_string(CHILD(nodes, node_idx));
                        asprintf(&str, buff, node_str);
                        node_idx += 1;
                    }
                    free(node_str);
                } else {
                    if (var_width) {
                        asprintf(&str, buff, CHILD(nodes, node_idx)->integer, CHILD(nodes, node_idx + 1)->_v);
                        node_idx += 2;
                    } else {
                        asprintf(&str, buff, CHILD(nodes, node_idx)->_v);
                        node_idx += 1;
                    }
                }
                array_push_n(chars, str, strlen(str));
                free(str);
            }
        } else {
            array_push(chars, c);
        }
        last = c;
        fmt += 1;
    }

    return array_data(chars);
}

/* Apply a function to arguments. */
static Node apply(Node *node) {
    Node        first;
    const char *name;
    Node        result;
    array_t     evaluated_nodes;
    Node       *it;
    Node       *it2;
    Node        elem;
    array_t    *lookup;
    array_t     fn_exprs;
    array_t     apply_args;

    if (array_len(node->children) < 1) {
        ERROR("no function to apply in empty list\n"
              "  did you mean to create an empty list? [list]\n");
    }

    first = interpret(CHILD(node->children, 0));

    if (first.kind != NAME_ATOM) {
        ERROR("expected function name as first element in list-function application\n");
    }

    name = first.name;

    /* check for special forms */
    if (strcmp(name, "if") == 0) {
        free_node(&first);
        return interpret_if(node);
    } else if (strcmp(name, "define") == 0) {
        free_node(&first);
        return interpret_define(node);
    }

    /* evaluate elements and apply function */
    evaluated_nodes = array_make(Node);
    array_push(evaluated_nodes, first);
    array_traverse_from(node->children, it, 1) {
        result = interpret(it);
        array_push(evaluated_nodes, result);
    }

    if (strcmp(name, "+") == 0) {
        check(&evaluated_nodes, 2, INT_ATOM, INT_ATOM);
        result = make_int(  CHILD(evaluated_nodes, 1)->integer
                          + CHILD(evaluated_nodes, 2)->integer);
    } else if (strcmp(name, "-") == 0) {
        check(&evaluated_nodes, 2, INT_ATOM, INT_ATOM);
        result = make_int(  CHILD(evaluated_nodes, 1)->integer
                          - CHILD(evaluated_nodes, 2)->integer);
    } else if (strcmp(name, "*") == 0) {
        check(&evaluated_nodes, 2, INT_ATOM, INT_ATOM);
        result = make_int(  CHILD(evaluated_nodes, 1)->integer
                          * CHILD(evaluated_nodes, 2)->integer);
    } else if (strcmp(name, "/") == 0) {
        check(&evaluated_nodes, 2, INT_ATOM, INT_ATOM);
        result = make_int(  CHILD(evaluated_nodes, 1)->integer
                          / CHILD(evaluated_nodes, 2)->integer);
    } else if (strcmp(name, "%") == 0) {
        check(&evaluated_nodes, 2, INT_ATOM, INT_ATOM);
        result = make_int(  CHILD(evaluated_nodes, 1)->integer
                          % CHILD(evaluated_nodes, 2)->integer);
    } else if (strcmp(name, "==") == 0) {
        check(&evaluated_nodes, 2, INT_ATOM, INT_ATOM);
        result = make_int(   CHILD(evaluated_nodes, 1)->integer
                          == CHILD(evaluated_nodes, 2)->integer);
    } else if (strcmp(name, "!=") == 0) {
        check(&evaluated_nodes, 2, INT_ATOM, INT_ATOM);
        result = make_int(   CHILD(evaluated_nodes, 1)->integer
                          != CHILD(evaluated_nodes, 2)->integer);
    } else if (strcmp(name, "<") == 0) {
        check(&evaluated_nodes, 2, INT_ATOM, INT_ATOM);
        result = make_int(  CHILD(evaluated_nodes, 1)->integer
                          < CHILD(evaluated_nodes, 2)->integer);
    } else if (strcmp(name, "<=") == 0) {
        check(&evaluated_nodes, 2, INT_ATOM, INT_ATOM);
        result = make_int(   CHILD(evaluated_nodes, 1)->integer
                          <= CHILD(evaluated_nodes, 2)->integer);
    } else if (strcmp(name, ">") == 0) {
        check(&evaluated_nodes, 2, INT_ATOM, INT_ATOM);
        result = make_int(  CHILD(evaluated_nodes, 1)->integer
                          > CHILD(evaluated_nodes, 2)->integer);
    } else if (strcmp(name, ">=") == 0) {
        check(&evaluated_nodes, 2, INT_ATOM, INT_ATOM);
        result = make_int(   CHILD(evaluated_nodes, 1)->integer
                          >= CHILD(evaluated_nodes, 2)->integer);
    } else if (strcmp(name, "list") == 0) {
        result = make_list();
        array_traverse_from(evaluated_nodes, it, 1) {
            elem = copy_node(it);
            array_push(result.children, elem);
        }
    } else if (strcmp(name, "len") == 0) {
        check(&evaluated_nodes, 1, LIST);
        result = make_int(array_len(CHILD(evaluated_nodes, 1)->children));
    } else if (strcmp(name, "append") == 0) {
        check(&evaluated_nodes, 2, LIST, LIST);
        result = make_list();
        array_traverse(CHILD(evaluated_nodes, 1)->children, it) {
            elem = copy_node(it);
            array_push(result.children, elem);
        }
        array_traverse(CHILD(evaluated_nodes, 2)->children, it) {
            elem = copy_node(it);
            array_push(result.children, elem);
        }
    } else if (strcmp(name, "car") == 0) {
        check(&evaluated_nodes, 1, LIST);
        it = CHILD(evaluated_nodes, 1);
        if (array_len(it->children) < 1) {
            ERROR("car expects a non-empty list\n");
        }
        result = copy_node(CHILD(it->children, 0));
    } else if (strcmp(name, "cdr") == 0) {
        check(&evaluated_nodes, 1, LIST);

        result = make_list();
        it     = CHILD(evaluated_nodes, 1);

        array_traverse_from(it->children, it2, 1) {
            elem = copy_node(it2);
            array_push(result.children, elem);
        }
    } else if (strcmp(name, "rand") == 0) {
        result = make_int(rand());
    } else if (strcmp(name, "print") == 0) {
        check(&evaluated_nodes, 1, -1);
        print_node(CHILD(evaluated_nodes, 1));
        result = copy_node(CHILD(evaluated_nodes, 1));
    } else if (strcmp(name, "fmt") == 0) {
        if (array_len(evaluated_nodes) < 2) {
            ERROR("fmt expects at least one argument\n");
        }
        if (CHILD(evaluated_nodes, 1)->kind != STRING_ATOM) {
            ERROR("first argument to fmt must be a string\n");
        }
        result = make_string_no_dup(do_fmt(evaluated_nodes));
    } else if (strcmp(name, "pfmt") == 0) {
        if (array_len(evaluated_nodes) < 2) {
            ERROR("pfmt expects at least one argument\n");
        }
        if (CHILD(evaluated_nodes, 1)->kind != STRING_ATOM) {
            ERROR("first argument to pfmt must be a string\n");
        }
        result = make_string_no_dup(do_fmt(evaluated_nodes));
        fwrite(result.string, 1, strlen(result.string), stdout);
    } else if ((lookup = hash_table_get_val(functions, name)) != NULL) {
        /* The function to apply isn't built in, but is found in our symbol
           table. */

        /* Push the arguments onto the stack so that argument references
           within the function are resolved properly. */
        apply_args = array_make(Node);
        array_traverse(evaluated_nodes, it) {
            elem = copy_node(it);
            array_push(apply_args, elem);
        }
        array_push(args, apply_args);

        /* Evaluated each expression in the function. */
        /* We have to deep copy the function since it can effectively
           delete its nodes if it redefines itself. */
        fn_exprs = array_make(Node);
        array_traverse(*lookup, it) {
            elem = copy_node(it);
            array_push(fn_exprs, elem);
        }

        result.kind = INVALID;
        array_traverse(fn_exprs, it) {
            if (result.kind != INVALID) {
                free_node(&result);
            }
            result = interpret(it);
            free_node(it);
        }
        array_free(fn_exprs);

        /* Remove the arguments from the stack. */
        array_pop(args);
        array_traverse(apply_args, it) {
            free_node(it);
        }
        array_free(apply_args);
    } else {
        ERROR("unknown function '%s'\n", name);
    }

    array_traverse(evaluated_nodes, it) {
        free_node(it);
    }
    array_free(evaluated_nodes);

    return result;
}

/* Entry point for interpretation of some unkown node. */
static Node interpret(Node *node) {
    Node       val;
    Node       tmp;
    Node      *it;
    long long  idx;
    char      *end;
    array_t   *apply_args;

    val.kind = INVALID;

    switch (node->kind) {
        case INVALID:
            ERROR("bad node!!!\n");
            break;
        case PROGRAM:
            array_traverse(node->children, it) {
                val = interpret(it);
                free_node(&val);
            }
            break;
        case LIST:
            /* Lists are always a function application. */
            val = apply(node);
            break;
        case INT_ATOM:
        case STRING_ATOM:
            val = copy_node(node);
            break;
        case NAME_ATOM:
            /* If a name starts with ':', it is an argument reference. */
            if (node->name[0] == ':') {
                if (array_len(args) == 0) {
                    ERROR("argument references are only valid within a function\n");
                }

                /* Grab the appropriate argument from the argument stack. */
                apply_args = array_last(args);
                idx = strtoll(node->name + 1, &end, 10);
                if (end == node->name + 1) {
                    ERROR("unable to parse argument index from '%s'\n", node->name);
                }

                if (array_len(*apply_args) <= idx) {
                    ERROR("argument reference invalid (%lld)\n", idx);
                }

                val = copy_node((Node*)array_item(*apply_args, idx));
            } else {
                val = copy_node(node);
            }
            break;
        default:
            break;
    }

    return val;
}

int main(int argc, char **argv) {
    Node node;

    if (argc != 2) {
        ERROR("USAGE: %s FILE\n", argv[0]);
    }

    if (!(cursor = mmap_file(argv[1]))) {
        ERROR("unable to open '%s'\n", argv[1]);
    }

    srand(time(NULL));

    /* Set up data structures. */
    functions = hash_table_make_e(fn_name_t, array_t, str_hash, str_equ);
    args      = array_make(array_t);
    program   = make_node(PROGRAM);

    /* Parse the whole file. */
    while ((node = parse_node()).kind != INVALID) {
        array_push(program.children, node);
    }

    /* Go! */
    interpret(&program);

    return 0;
}
