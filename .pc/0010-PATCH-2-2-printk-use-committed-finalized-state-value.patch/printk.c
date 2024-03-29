#include "makedumpfile.h"
#include <ctype.h>

#define DESC_SV_BITS		(sizeof(unsigned long) * 8)
#define DESC_COMMITTED_MASK	(1UL << (DESC_SV_BITS - 1))
#define DESC_REUSE_MASK		(1UL << (DESC_SV_BITS - 2))
#define DESC_FLAGS_MASK		(DESC_COMMITTED_MASK | DESC_REUSE_MASK)
#define DESC_ID_MASK		(~DESC_FLAGS_MASK)

/* convenience struct for passing many values to helper functions */
struct prb_map {
	char		*prb;

	char		*desc_ring;
	unsigned long	desc_ring_count;
	char		*descs;
	char		*infos;

	char		*text_data_ring;
	unsigned long	text_data_ring_size;
	char		*text_data;
};

static void
dump_record(struct prb_map *m, unsigned long id)
{
	unsigned long long ts_nsec;
	unsigned long state_var;
	unsigned short text_len;
	unsigned long begin;
	unsigned long next;
	char buf[BUFSIZE];
	ulonglong nanos;
	int indent_len;
	int buf_need;
	char *bufp;
	char *text;
	char *desc;
	char *inf;
	ulong rem;
	char *p;
	int i;

	desc = m->descs + ((id % m->desc_ring_count) * SIZE(prb_desc));

	/* skip non-committed record */
	state_var = ULONG(desc + OFFSET(prb_desc.state_var) + OFFSET(atomic_long_t.counter));
	if ((state_var & DESC_FLAGS_MASK) != DESC_COMMITTED_MASK)
		return;

	begin = ULONG(desc + OFFSET(prb_desc.text_blk_lpos) + OFFSET(prb_data_blk_lpos.begin)) %
			m->text_data_ring_size;
	next = ULONG(desc + OFFSET(prb_desc.text_blk_lpos) + OFFSET(prb_data_blk_lpos.next)) %
			m->text_data_ring_size;

	/* skip data-less text blocks */
	if (begin == next)
		return;

	inf = m->infos + ((id % m->desc_ring_count) * SIZE(printk_info));

	text_len = USHORT(inf + OFFSET(printk_info.text_len));

	/* handle wrapping data block */
	if (begin > next)
		begin = 0;

	/* skip over descriptor ID */
	begin += sizeof(unsigned long);

	/* handle truncated messages */
	if (next - begin < text_len)
		text_len = next - begin;

	text = m->text_data + begin;

	ts_nsec = ULONGLONG(inf + OFFSET(printk_info.ts_nsec));
	nanos = (ulonglong)ts_nsec / (ulonglong)1000000000;
	rem = (ulonglong)ts_nsec % (ulonglong)1000000000;

	bufp = buf;
	bufp += sprintf(buf, "[%5lld.%06ld] ", nanos, rem/1000);
	indent_len = strlen(buf);

	/* How much buffer space is needed in the worst case */
	buf_need = MAX(sizeof("\\xXX\n"), sizeof("\n") + indent_len);

	for (i = 0, p = text; i < text_len; i++, p++) {
		if (bufp - buf >= sizeof(buf) - buf_need) {
			if (write(info->fd_dumpfile, buf, bufp - buf) < 0)
				return;
			bufp = buf;
		}

		if (*p == '\n')
			bufp += sprintf(bufp, "\n%-*s", indent_len, "");
		else if (isprint(*p) || isspace(*p))
			*bufp++ = *p;
		else
			bufp += sprintf(bufp, "\\x%02x", *p);
	}

	*bufp++ = '\n';

	write(info->fd_dumpfile, buf, bufp - buf);
}

int
dump_lockless_dmesg(void)
{
	unsigned long head_id;
	unsigned long tail_id;
	unsigned long kaddr;
	unsigned long id;
	struct prb_map m;
	int ret = FALSE;

	/* setup printk_ringbuffer */
	if (!readmem(VADDR, SYMBOL(prb), &kaddr, sizeof(kaddr))) {
		ERRMSG("Can't get the prb address.\n");
		return ret;
	}

	m.prb = malloc(SIZE(printk_ringbuffer));
	if (!m.prb) {
		ERRMSG("Can't allocate memory for prb.\n");
		return ret;
	}
	if (!readmem(VADDR, kaddr, m.prb, SIZE(printk_ringbuffer))) {
		ERRMSG("Can't get prb.\n");
		goto out_prb;
	}

	/* setup descriptor ring */
	m.desc_ring = m.prb + OFFSET(printk_ringbuffer.desc_ring);
	m.desc_ring_count = 1 << UINT(m.desc_ring + OFFSET(prb_desc_ring.count_bits));

	kaddr = ULONG(m.desc_ring + OFFSET(prb_desc_ring.descs));
	m.descs = malloc(SIZE(prb_desc) * m.desc_ring_count);
	if (!m.descs) {
		ERRMSG("Can't allocate memory for prb.desc_ring.descs.\n");
		goto out_prb;
	}
	if (!readmem(VADDR, kaddr, m.descs,
		     SIZE(prb_desc) * m.desc_ring_count)) {
		ERRMSG("Can't get prb.desc_ring.descs.\n");
		goto out_descs;
	}

	kaddr = ULONG(m.desc_ring + OFFSET(prb_desc_ring.infos));
	m.infos = malloc(SIZE(printk_info) * m.desc_ring_count);
	if (!m.infos) {
		ERRMSG("Can't allocate memory for prb.desc_ring.infos.\n");
		goto out_descs;
	}
	if (!readmem(VADDR, kaddr, m.infos, SIZE(printk_info) * m.desc_ring_count)) {
		ERRMSG("Can't get prb.desc_ring.infos.\n");
		goto out_infos;
	}

	/* setup text data ring */
	m.text_data_ring = m.prb + OFFSET(printk_ringbuffer.text_data_ring);
	m.text_data_ring_size = 1 << UINT(m.text_data_ring + OFFSET(prb_data_ring.size_bits));

	kaddr = ULONG(m.text_data_ring + OFFSET(prb_data_ring.data));
	m.text_data = malloc(m.text_data_ring_size);
	if (!m.text_data) {
		ERRMSG("Can't allocate memory for prb.text_data_ring.data.\n");
		goto out_infos;
	}
	if (!readmem(VADDR, kaddr, m.text_data, m.text_data_ring_size)) {
		ERRMSG("Can't get prb.text_data_ring.\n");
		goto out_text_data;
	}

	/* ready to go */

	tail_id = ULONG(m.desc_ring + OFFSET(prb_desc_ring.tail_id) +
			OFFSET(atomic_long_t.counter));
	head_id = ULONG(m.desc_ring + OFFSET(prb_desc_ring.head_id) +
			OFFSET(atomic_long_t.counter));

	if (!open_dump_file()) {
		ERRMSG("Can't open output file.\n");
		goto out_text_data;
	}

	for (id = tail_id; id != head_id; id = (id + 1) & DESC_ID_MASK)
		dump_record(&m, id);

	/* dump head record */
	dump_record(&m, id);

	if (!close_files_for_creating_dumpfile())
		goto out_text_data;

	ret = TRUE;
out_text_data:
	free(m.text_data);
out_infos:
	free(m.infos);
out_descs:
	free(m.descs);
out_prb:
	free(m.prb);
	return ret;
}
