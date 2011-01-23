#include "../sl.h"
#include <stdlib.h>

struct node_top {
    struct node_top *next;
    struct node_low *data;
};

struct node_low {
    struct node_low *next;
};

struct node_top* create_top(void)
{
    struct node_top *ptr = malloc(sizeof *ptr);
    if (!ptr)
        abort();

    ptr->next = NULL;
    ptr->data = NULL;

    return ptr;
}

struct node_low* create_low(void)
{
    struct node_low *ptr = malloc(sizeof *ptr);
    if (!ptr)
        abort();

    ptr->next = NULL;

    return ptr;
}

int get_nondet()
{
    int i;
    return i;
}

struct node_top* alloc(void)
{
    struct node_top *pi = create_top();
    if (get_nondet())
        return pi;

    pi->data = create_low();

    while (get_nondet()) {
        struct node_low *low = create_low();
        low->next = pi->data;
        pi->data = low;
    }

    return pi;
}

struct node_top* create_sll(void)
{
    struct node_top *sll = alloc();
    struct node_top *now = sll;

    // NOTE: running this on bare metal may cause the machine to swap a bit
    int i;
    for (i = 1; i; ++i) {
        now->next = alloc();
        now = now->next;
    }

    return sll;
}

int main()
{
    struct node_top *sll = create_sll();
    ___sl_plot_by_ptr(&sll, NULL);

    return 0;
}