#ifndef _SHEEP_CODE_H
#define _SHEEP_CODE_H

#include <sheep/vector.h>

enum sheep_opcode {
	SHEEP_DROP,
	SHEEP_LOCAL,
	SHEEP_SET_LOCAL,
	SHEEP_FOREIGN,
	SHEEP_SET_FOREIGN,
	SHEEP_GLOBAL,
	SHEEP_SET_GLOBAL,
	SHEEP_CLOSURE,
	SHEEP_CALL,
	SHEEP_RET,
};

#define SHEEP_OPCODE_BITS	5
#define SHEEP_OPCODE_SHIFT	(sizeof(long) * 8 - SHEEP_OPCODE_BITS)

struct sheep_code {
	struct sheep_vector code;
};

static inline unsigned long
sheep_encode(enum sheep_opcode op, unsigned int arg)
{
	unsigned long code;

	code = (unsigned long)op << SHEEP_OPCODE_SHIFT;
	code |= arg;
	return code;
}

static inline void
sheep_decode(unsigned long code, enum sheep_opcode *op, unsigned int *arg)
{
	*op = code >> SHEEP_OPCODE_SHIFT;
	*arg = code & ((1UL << SHEEP_OPCODE_SHIFT) - 1);
}

static inline void
sheep_emit(struct sheep_code *code, enum sheep_opcode op, unsigned int arg)
{
	sheep_vector_push(&code->code, (void *)sheep_encode(op, arg));
}

#endif /* _SHEEP_CODE_H */
