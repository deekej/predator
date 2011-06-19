#include "../sl.h"
#include <stdlib.h>

struct lowLevel {
    struct lowLevel *head;
    struct lowLevel *next;
};

struct topLevel {
    struct topLevel *next;
    struct lowLevel *low;
};

int main()
{
    struct topLevel *t, *tt;
    struct lowLevel *l;

    t = malloc(sizeof(struct topLevel));
    t->next = 0;
    t->low = 0;

    while (___sl_get_nondet_int()) {
        do {
            l = malloc(sizeof(struct lowLevel));
            if (!l)
                abort();

            if (t->low == 0) {
                l->head = l;
                l->next = 0;
                t->low = l;
            }
            else {
                l->head = t->low;
                l->next = t->low->next;
                t->low->next = l;
            }
        }
        while (___sl_get_nondet_int());

        tt = malloc(sizeof(struct topLevel));
        tt->next = t;
        tt->low = 0;
        t = tt;
    }

    tt = t->next;
    l = tt->low;

    return 0;
}