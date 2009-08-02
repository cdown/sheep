#ifndef _SHEEP_NAME_H
#define _SHEEP_NAME_H

#include <sheep/object.h>

extern const struct sheep_type sheep_name_type;

sheep_t sheep_name(struct sheep_vm *, const char *);

static inline const char *sheep_cname(sheep_t sheep)
{
	return (const char *)sheep->data;
}

#endif /* _SHEEP_NAME_H */
