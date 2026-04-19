#include "reflection.h"

#include <stdio.h>

static void fprint_type(FILE *fp, const AstNode *t);

static void fprint_type_list(FILE *fp, const AstList *list, const char *sep) {
    const AstList *l;
    int first = 1;
    for (l = list; l; l = l->next) {
        if (!first) {
            fputs(sep, fp);
        }
        first = 0;
        fprint_type(fp, l->item);
    }
}

static void fprint_type(FILE *fp, const AstNode *t) {
    if (!t) {
        fputs("?", fp);
        return;
    }
    switch (t->kind) {
        case NODE_TYPE_PRIMITIVE:
            switch (AS_TYPE_PRIM(t).prim) {
                case PRIM_INT:
                    fputs("int", fp);
                    break;
                case PRIM_FLOAT:
                    fputs("float", fp);
                    break;
                case PRIM_DOUBLE:
                    fputs("double", fp);
                    break;
                case PRIM_BOOL:
                    fputs("bool", fp);
                    break;
                case PRIM_STRING:
                    fputs("string", fp);
                    break;
                default:
                    fputs("?", fp);
                    break;
            }
            break;
        case NODE_TYPE_NAMED:
            fputs(AS_TYPE_NAMED(t).name, fp);
            if (AS_TYPE_NAMED(t).type_args) {
                fputc('[', fp);
                fprint_type_list(fp, AS_TYPE_NAMED(t).type_args, ", ");
                fputc(']', fp);
            }
            break;
        case NODE_TYPE_SELF:
            fputs("Self", fp);
            break;
        case NODE_TYPE_ARRAY:
            fputc('[', fp);
            fprint_type(fp, AS_TYPE_ARRAY(t).elem_type);
            fputc(']', fp);
            break;
        case NODE_TYPE_FN:
            fputs("fn(", fp);
            fprint_type_list(fp, AS_TYPE_FN(t).param_types, ", ");
            fputs(") -> ", fp);
            if (AS_TYPE_FN(t).ret_type) {
                fprint_type(fp, AS_TYPE_FN(t).ret_type);
            } else {
                fputs("()", fp);
            }
            break;
        case NODE_TYPE_OPTION:
            fputs("Option[", fp);
            fprint_type(fp, AS_TYPE_OPTION(t).inner);
            fputc(']', fp);
            break;
        case NODE_TYPE_RESULT:
            fputs("Result[", fp);
            fprint_type(fp, AS_TYPE_RESULT(t).ok_type);
            fputs(", ", fp);
            fprint_type(fp, AS_TYPE_RESULT(t).err_type);
            fputc(']', fp);
            break;
        case NODE_TYPE_TUPLE:
            fputc('(', fp);
            fprint_type_list(fp, AS_TYPE_TUPLE(t).elem_types, ", ");
            fputc(')', fp);
            break;
        case NODE_TYPE_REF:
            fputs(AS_TYPE_REF(t).is_mut ? "&mut " : "&", fp);
            fprint_type(fp, AS_TYPE_REF(t).inner);
            break;
        default:
            fputs("?", fp);
            break;
    }
}

static void fprint_param(FILE *fp, const AstNode *p) {
    if (p->kind != NODE_PARAM) {
        fputs("?", fp);
        return;
    }
    if (AS_PARAM(p).is_mut) {
        fputs("mut ", fp);
    }
    fputs(AS_PARAM(p).name, fp);
    fputc(':', fp);
    fprint_type(fp, AS_PARAM(p).type);
}

void reflection_fprint_program(FILE *fp, const AstNode *program) {
    AstList *d;

    if (!program || program->kind != NODE_PROGRAM) {
        fputs("(no program)\n", fp);
        return;
    }

    fputs("# structs\n", fp);
    for (d = AS_PROGRAM(program).decls; d; d = d->next) {
        AstNode *n = d->item;
        AstList *fl;
        if (n->kind != NODE_STRUCT_DECL || !AS_STRUCT_DECL(n).is_pub) {
            continue;
        }
        fprintf(fp, "struct %s\n", AS_STRUCT_DECL(n).name);
        for (fl = AS_STRUCT_DECL(n).fields; fl; fl = fl->next) {
            AstNode *f = fl->item;
            if (f->kind != NODE_STRUCT_FIELD) {
                continue;
            }
            fputc(' ', fp);
            if (AS_STRUCT_FIELD(f).is_pub) {
                fputs("pub ", fp);
            }
            if (AS_STRUCT_FIELD(f).is_mut) {
                fputs("mut ", fp);
            }
            fputs(AS_STRUCT_FIELD(f).name, fp);
            fputs(": ", fp);
            fprint_type(fp, AS_STRUCT_FIELD(f).type);
            fputc('\n', fp);
        }
    }

    fputs("\n# pub fn\n", fp);
    for (d = AS_PROGRAM(program).decls; d; d = d->next) {
        AstNode *n = d->item;
        AstList *pl;
        if (n->kind != NODE_FN_DECL || !AS_FN_DECL(n).is_pub) {
            continue;
        }
        fputs("pub fn ", fp);
        fputs(AS_FN_DECL(n).name, fp);
        fputc('(', fp);
        for (pl = AS_FN_DECL(n).params; pl; pl = pl->next) {
            if (pl != AS_FN_DECL(n).params) {
                fputs(", ", fp);
            }
            fprint_param(fp, pl->item);
        }
        fputs(")", fp);
        if (AS_FN_DECL(n).ret_type) {
            fputs(" -> ", fp);
            fprint_type(fp, AS_FN_DECL(n).ret_type);
        }
        fputc('\n', fp);
    }

    fputs("\n# pub const\n", fp);
    for (d = AS_PROGRAM(program).decls; d; d = d->next) {
        AstNode *n = d->item;
        if (n->kind != NODE_CONST_DECL || !AS_CONST_DECL(n).is_pub) {
            continue;
        }
        fputs("pub const ", fp);
        fputs(AS_CONST_DECL(n).name, fp);
        fputs(": ", fp);
        fprint_type(fp, AS_CONST_DECL(n).type);
        fputc('\n', fp);
    }
}
