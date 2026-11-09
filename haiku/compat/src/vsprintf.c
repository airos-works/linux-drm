/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Linux's printf: the C conversions plus the kernel's %p extensions the
	drivers use (%pV %pe %pa %pad %ph %p4cc %pS %pM %pU ...), printk to
	Haiku's syslog, warnings and panics, and sscanf. */

#include <lx/haiku.h>


struct out {
	char *buf;
	size_t size;
	size_t len;		/* would-be length */
};


static void
out_char(struct out *o, char c)
{
	if (o->len + 1 < o->size)
		o->buf[o->len] = c;
	o->len++;
}


static void
out_string(struct out *o, const char *s, int width, int precision, bool left)
{
	int length = 0, pad;
	if (s == NULL)
		s = "(null)";
	while ((precision < 0 || length < precision) && s[length] != '\0')
		length++;
	pad = width > length ? width - length : 0;
	if (!left)
		while (pad-- > 0)
			out_char(o, ' ');
	while (length-- > 0)
		out_char(o, *s++);
	if (left)
		while (pad-- > 0)
			out_char(o, ' ');
}


#define F_LEFT	1
#define F_PLUS	2
#define F_SPACE	4
#define F_ALT	8
#define F_ZERO	16
#define F_UPPER	32
#define F_SIGNED 64

static void
out_number(struct out *o, unsigned long long value, int base, int flags,
	int width, int precision)
{
	char digits[72];
	const char *set = (flags & F_UPPER) ? "0123456789ABCDEF" : "0123456789abcdef";
	char sign = 0;
	int count = 0, length, pad, zeros;
	const char *prefix = "";

	if ((flags & F_SIGNED) && (long long)value < 0) {
		sign = '-';
		value = -(long long)value;
	} else if (flags & F_SIGNED) {
		if (flags & F_PLUS)
			sign = '+';
		else if (flags & F_SPACE)
			sign = ' ';
	}
	if ((flags & F_ALT) && value != 0) {
		if (base == 16)
			prefix = (flags & F_UPPER) ? "0X" : "0x";
		else if (base == 8)
			prefix = "0";
	}
	if (value == 0 && precision != 0)
		digits[count++] = '0';
	while (value != 0) {
		digits[count++] = set[value % base];
		value /= base;
	}
	zeros = precision > count ? precision - count : 0;
	length = count + zeros + (sign ? 1 : 0) + (int)strlen(prefix);
	pad = width > length ? width - length : 0;
	if ((flags & F_ZERO) && !(flags & F_LEFT) && precision < 0) {
		zeros += pad;
		pad = 0;
	}
	if (!(flags & F_LEFT))
		while (pad-- > 0)
			out_char(o, ' ');
	if (sign)
		out_char(o, sign);
	while (*prefix)
		out_char(o, *prefix++);
	while (zeros-- > 0)
		out_char(o, '0');
	while (count-- > 0)
		out_char(o, digits[count]);
	if (flags & F_LEFT)
		while (pad-- > 0)
			out_char(o, ' ');
}


static const char *
error_name(int error)
{
	switch (error) {
		case -EPERM: return "-EPERM";
		case -ENOENT: return "-ENOENT";
		case -EINTR: return "-EINTR";
		case -EIO: return "-EIO";
		case -ENXIO: return "-ENXIO";
		case -E2BIG: return "-E2BIG";
		case -EBADF: return "-EBADF";
		case -EAGAIN: return "-EAGAIN";
		case -ENOMEM: return "-ENOMEM";
		case -EACCES: return "-EACCES";
		case -EFAULT: return "-EFAULT";
		case -EBUSY: return "-EBUSY";
		case -EEXIST: return "-EEXIST";
		case -ENODEV: return "-ENODEV";
		case -EINVAL: return "-EINVAL";
		case -ENOSPC: return "-ENOSPC";
		case -ERANGE: return "-ERANGE";
		case -EDEADLK: return "-EDEADLK";
		case -ENOSYS: return "-ENOSYS";
		case -ENODATA: return "-ENODATA";
		case -ETIME: return "-ETIME";
		case -EPROTO: return "-EPROTO";
		case -EOVERFLOW: return "-EOVERFLOW";
		case -ETIMEDOUT: return "-ETIMEDOUT";
		case -EALREADY: return "-EALREADY";
		case -ECANCELED: return "-ECANCELED";
		case -ENOTSUPP: return "-ENOTSUPP";
		case -EOPNOTSUPP: return "-EOPNOTSUPP";
		case -EPROBE_DEFER: return "-EPROBE_DEFER";
		case -ERESTARTSYS: return "-ERESTARTSYS";
		case -ENOTTY: return "-ENOTTY";
		case -EHWPOISON: return "-EHWPOISON";
		case -ENOLINK: return "-ENOLINK";
		case -ECOMM: return "-ECOMM";
	}
	return NULL;
}


static void format(struct out *o, const char *fmt, va_list args);

/* %p and its extensions; returns the format past the extension letters */
static const char *
out_pointer(struct out *o, const char *fmt, void *ptr, int flags, int width,
	int precision)
{
	char buffer[64];

	switch (*fmt) {
		case 'V': {
			struct va_format *vaf = ptr;
			va_list va;
			va_copy(va, *vaf->va);
			format(o, vaf->fmt, va);
			va_end(va);
			return fmt + 1;
		}
		case 'e': {
			long error = PTR_ERR(ptr);
			const char *name = IS_ERR(ptr) ? error_name(error) : NULL;
			if (name != NULL)
				out_string(o, name, width, -1, flags & F_LEFT);
			else
				out_number(o, (unsigned long long)error, 10, F_SIGNED, width, -1);
			return fmt + 1;
		}
		case 'a': {
			unsigned long long value;
			if (fmt[1] == 'd') {
				value = *(dma_addr_t *)ptr;
				fmt++;
			} else {
				value = *(phys_addr_t *)ptr;
				if (fmt[1] == 'p')
					fmt++;
			}
			out_number(o, value, 16, F_ALT | F_ZERO, 18, -1);
			return fmt + 1;
		}
		case 'h': {
			const u8 *bytes = ptr;
			char separator = ' ';
			int i, count = width > 0 ? min(width, 64) : 1;
			fmt++;
			if (*fmt == 'C') {
				separator = ':';
				fmt++;
			} else if (*fmt == 'D') {
				separator = '-';
				fmt++;
			} else if (*fmt == 'N') {
				separator = 0;
				fmt++;
			}
			for (i = 0; i < count; i++) {
				if (i > 0 && separator)
					out_char(o, separator);
				out_char(o, hex_asc_hi(bytes[i]));
				out_char(o, hex_asc_lo(bytes[i]));
			}
			return fmt;
		}
		case '4':
			if (fmt[1] == 'c' && fmt[2] == 'c') {
				u32 code = *(u32 *)ptr;
				int i;
				for (i = 0; i < 4; i++) {
					char c = (code >> (8 * i)) & 0x7f;
					out_char(o, isascii(c) && isprint(c) ? c : '.');
				}
				snprintf(buffer, sizeof(buffer), " %s-endian (0x%08x)",
					(code & BIT(31)) ? "big" : "little", code);
				out_string(o, buffer, 0, -1, false);
				return fmt + 3;
			}
			break;
		case 'M':
		case 'm': {
			const u8 *mac = ptr;
			snprintf(buffer, sizeof(buffer), "%02x:%02x:%02x:%02x:%02x:%02x",
				mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
			out_string(o, buffer, width, -1, flags & F_LEFT);
			fmt++;
			while (*fmt == 'F' || *fmt == 'R')
				fmt++;
			return fmt;
		}
		case 'U': {
			const u8 *u = ptr;
			snprintf(buffer, sizeof(buffer),
				"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
				u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9],
				u[10], u[11], u[12], u[13], u[14], u[15]);
			out_string(o, buffer, width, -1, flags & F_LEFT);
			fmt++;
			if (*fmt == 'b' || *fmt == 'B' || *fmt == 'l' || *fmt == 'L')
				fmt++;
			return fmt;
		}
		case 'R':
		case 'r': {
			const struct resource *res = ptr;
			snprintf(buffer, sizeof(buffer), "[mem %#010llx-%#010llx]",
				(unsigned long long)res->start, (unsigned long long)res->end);
			out_string(o, buffer, width, -1, flags & F_LEFT);
			return fmt + 1;
		}
		case 'D':
		case 'd':
			out_string(o, "(file)", width, -1, flags & F_LEFT);
			fmt++;
			while (isdigit(*fmt))
				fmt++;
			return fmt;
	}

	/* %p, %px, %pK, %pS, %ps, %pB, %pF, %pf, %pOF, %pt... : the address */
	out_number(o, (unsigned long)ptr, 16, F_ALT | F_ZERO, 18, -1);
	while (isalnum(*fmt))
		fmt++;
	return fmt;
}


static void
format(struct out *o, const char *fmt, va_list args)
{
	while (*fmt != '\0') {
		int flags = 0, width = -1, precision = -1;
		char length = 0;
		bool longlong = false;

		if (*fmt != '%') {
			out_char(o, *fmt++);
			continue;
		}
		fmt++;
		if (*fmt == '%') {
			out_char(o, '%');
			fmt++;
			continue;
		}

		for (;; fmt++) {
			if (*fmt == '-')
				flags |= F_LEFT;
			else if (*fmt == '+')
				flags |= F_PLUS;
			else if (*fmt == ' ')
				flags |= F_SPACE;
			else if (*fmt == '#')
				flags |= F_ALT;
			else if (*fmt == '0')
				flags |= F_ZERO;
			else
				break;
		}
		if (*fmt == '*') {
			width = va_arg(args, int);
			if (width < 0) {
				flags |= F_LEFT;
				width = -width;
			}
			fmt++;
		} else if (isdigit(*fmt)) {
			width = 0;
			while (isdigit(*fmt))
				width = width * 10 + (*fmt++ - '0');
		}
		if (*fmt == '.') {
			fmt++;
			precision = 0;
			if (*fmt == '*') {
				precision = va_arg(args, int);
				fmt++;
			} else {
				while (isdigit(*fmt))
					precision = precision * 10 + (*fmt++ - '0');
			}
		}
		if (*fmt == 'h') {
			length = 'h';
			fmt++;
			if (*fmt == 'h') {
				length = 'H';
				fmt++;
			}
		} else if (*fmt == 'l') {
			length = 'l';
			fmt++;
			if (*fmt == 'l') {
				longlong = true;
				fmt++;
			}
		} else if (*fmt == 'L' || *fmt == 'q') {
			longlong = true;
			fmt++;
		} else if (*fmt == 'z' || *fmt == 'Z' || *fmt == 't' || *fmt == 'j') {
			length = 'l';
			fmt++;
		}

		switch (*fmt) {
			case 'd':
			case 'i': {
				long long value;
				if (longlong)
					value = va_arg(args, long long);
				else if (length == 'l')
					value = va_arg(args, long);
				else
					value = va_arg(args, int);
				if (length == 'h')
					value = (short)value;
				else if (length == 'H')
					value = (signed char)value;
				out_number(o, value, 10, flags | F_SIGNED, width, precision);
				break;
			}
			case 'u':
			case 'x':
			case 'X':
			case 'o': {
				unsigned long long value;
				int base = *fmt == 'u' ? 10 : *fmt == 'o' ? 8 : 16;
				if (longlong)
					value = va_arg(args, unsigned long long);
				else if (length == 'l')
					value = va_arg(args, unsigned long);
				else
					value = va_arg(args, unsigned int);
				if (length == 'h')
					value = (unsigned short)value;
				else if (length == 'H')
					value = (unsigned char)value;
				out_number(o, value, base, flags | (*fmt == 'X' ? F_UPPER : 0),
					width, precision);
				break;
			}
			case 'c': {
				char c = va_arg(args, int);
				int pad = width > 1 ? width - 1 : 0;
				if (!(flags & F_LEFT))
					while (pad-- > 0)
						out_char(o, ' ');
				out_char(o, c);
				if (flags & F_LEFT)
					while (pad-- > 0)
						out_char(o, ' ');
				break;
			}
			case 's':
				out_string(o, va_arg(args, const char *), width, precision,
					flags & F_LEFT);
				break;
			case 'p':
				fmt = out_pointer(o, fmt + 1, va_arg(args, void *), flags, width,
					precision);
				continue;
			case 'n':
				*va_arg(args, int *) = o->len;
				break;
			case '\0':
				return;
			default:
				out_char(o, '%');
				out_char(o, *fmt);
				break;
		}
		fmt++;
	}
}


int
vsnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
	struct out o = { buf, size, 0 };
	va_list va;
	va_copy(va, args);
	format(&o, fmt, va);
	va_end(va);
	if (size > 0)
		buf[min(o.len, size - 1)] = '\0';
	return o.len;
}


int
snprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list args;
	int length;
	va_start(args, fmt);
	length = vsnprintf(buf, size, fmt, args);
	va_end(args);
	return length;
}


int
vscnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
	int length = vsnprintf(buf, size, fmt, args);
	if (likely(length < (int)size))
		return length;
	return size != 0 ? size - 1 : 0;
}


int
scnprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list args;
	int length;
	va_start(args, fmt);
	length = vscnprintf(buf, size, fmt, args);
	va_end(args);
	return length;
}


int
vsprintf(char *buf, const char *fmt, va_list args)
{
	return vsnprintf(buf, INT_MAX, fmt, args);
}


int
sprintf(char *buf, const char *fmt, ...)
{
	va_list args;
	int length;
	va_start(args, fmt);
	length = vsnprintf(buf, INT_MAX, fmt, args);
	va_end(args);
	return length;
}


char *
kvasprintf(unsigned int gfp, const char *fmt, va_list args)
{
	int length = vsnprintf(NULL, 0, fmt, args);
	char *p = kmalloc(length + 1, gfp);
	if (p == NULL)
		return NULL;
	vsnprintf(p, length + 1, fmt, args);
	return p;
}


const char *
kvasprintf_const(unsigned int gfp, const char *fmt, va_list args)
{
	return kvasprintf(gfp, fmt, args);
}


char *
kasprintf(unsigned int gfp, const char *fmt, ...)
{
	va_list args;
	char *p;
	va_start(args, fmt);
	p = kvasprintf(gfp, fmt, args);
	va_end(args);
	return p;
}


/* #pragma mark - printk */

/*	Bring-up aid: sleep this long after every message, so that the syslog
	daemon has written the messages before a step that may stop the machine
	(settings: "printk_delay_us 20000"). */
static int printk_delay_us;
module_param(printk_delay_us, int, 0644);

/*	Bring-up aid: also write every message to this file and sync it, so the
	messages before a crash survive the power cycle (settings:
	"log_file /boot/home/amdgpu_drm.log"). The file must exist, filled with
	zeros (x399-arm.sh makes it): it is overwritten, never extended. */
static char *log_file;
module_param(log_file, charp, 0444);


int
vprintk(const char *fmt, va_list args)
{
	static char *opened;
	if (unlikely(log_file != opened)) {
		opened = log_file;
		lx_haiku_log_file(log_file);
	}
	char buffer[1024];
	const char *text = buffer;
	int length = vsnprintf(buffer, sizeof(buffer) - 1, fmt, args);
	/* KERN_<level>: "\001" and a digit or 'c' */
	while (text[0] == '\001' && text[1] != '\0')
		text += 2;
#ifdef LX_ADDON_AMDXDNA
	/* amdxdna's messages do not end their lines: Linux's printk() ends a
	 * line when the next message is not a continuation (KERN_CONT), which
	 * this driver does not use */
	if (*text != '\0' && text[strlen(text) - 1] != '\n')
		strcat(buffer, "\n");
#endif
	if (*text != '\0')
		lx_haiku_log(text);
	if (printk_delay_us > 0)
		lx_haiku_snooze(printk_delay_us);
	return length;
}


int
_printk(const char *fmt, ...)
{
	va_list args;
	int length;
	va_start(args, fmt);
	length = vprintk(fmt, args);
	va_end(args);
	return length;
}


void
_dev_printk(const char *level, const struct device *dev, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;
	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	_printk("%s%s %s: %pV", level, KBUILD_MODNAME,
		dev != NULL ? dev_name(dev) : "(no device)", &vaf);
	va_end(args);
}


void
dump_stack(void)
{
	lx_haiku_dump_stack();
}


void
lx_warn(const char *file, int line, const char *func, const char *condition)
{
	_printk(KERN_WARNING "WARNING: %s:%d %s(): %s\n", kbasename(file), line,
		func, condition);
}


void
lx_warn_printf(const char *file, int line, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;
	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	_printk(KERN_WARNING "WARNING: %s:%d: %pV", kbasename(file), line, &vaf);
	va_end(args);
}


void
lx_panic(const char *fmt, ...)
{
	char buffer[512];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);
	lx_haiku_panic(buffer);
}


/* #pragma mark - numbers from strings */


unsigned long long
simple_strtoull(const char *cp, char **endp, unsigned int base)
{
	unsigned long long result = 0;
	if (base == 0) {
		base = 10;
		if (cp[0] == '0') {
			base = 8;
			if (_tolower(cp[1]) == 'x' && isxdigit(cp[2])) {
				base = 16;
				cp += 2;
			}
		}
	} else if (base == 16 && cp[0] == '0' && _tolower(cp[1]) == 'x')
		cp += 2;
	while (isxdigit(*cp)) {
		unsigned int value = isdigit(*cp) ? *cp - '0'
			: _tolower(*cp) - 'a' + 10;
		if (value >= base)
			break;
		result = result * base + value;
		cp++;
	}
	if (endp)
		*endp = (char *)cp;
	return result;
}


unsigned long
simple_strtoul(const char *cp, char **endp, unsigned int base)
{
	return simple_strtoull(cp, endp, base);
}


long
simple_strtol(const char *cp, char **endp, unsigned int base)
{
	if (*cp == '-')
		return -(long)simple_strtoull(cp + 1, endp, base);
	return simple_strtoull(cp, endp, base);
}


long long
simple_strtoll(const char *cp, char **endp, unsigned int base)
{
	if (*cp == '-')
		return -(long long)simple_strtoull(cp + 1, endp, base);
	return simple_strtoull(cp, endp, base);
}


/* a sscanf for the integer, string and character conversions */
int
vsscanf(const char *buf, const char *fmt, va_list args)
{
	const char *str = buf;
	int count = 0;

	while (*fmt != '\0') {
		if (isspace(*fmt)) {
			while (isspace(*fmt))
				fmt++;
			while (isspace(*str))
				str++;
			continue;
		}
		if (*fmt != '%' || fmt[1] == '%') {
			if (*fmt == '%')
				fmt++;
			if (*fmt != *str)
				break;
			fmt++;
			str++;
			continue;
		}
		fmt++;
		{
			bool skip = false;
			int width = -1;
			char length = 0;
			if (*fmt == '*') {
				skip = true;
				fmt++;
			}
			if (isdigit(*fmt)) {
				width = 0;
				while (isdigit(*fmt))
					width = width * 10 + (*fmt++ - '0');
			}
			if (*fmt == 'h') {
				length = 'h';
				fmt++;
				if (*fmt == 'h') {
					length = 'H';
					fmt++;
				}
			} else if (*fmt == 'l') {
				length = 'l';
				fmt++;
				if (*fmt == 'l') {
					length = 'L';
					fmt++;
				}
			} else if (*fmt == 'z') {
				length = 'l';
				fmt++;
			}

			if (*fmt == 'n') {
				if (!skip)
					*va_arg(args, int *) = str - buf;
				fmt++;
				continue;
			}
			if (*fmt != 'c')
				while (isspace(*str))
					str++;
			if (*str == '\0')
				break;

			if (*fmt == 's' || *fmt == 'c') {
				char *dst = skip ? NULL : va_arg(args, char *);
				int n = 0;
				if (*fmt == 'c' && width < 0)
					width = 1;
				while (*str != '\0' && (width < 0 || n < width)
					&& (*fmt == 'c' || !isspace(*str))) {
					if (dst)
						dst[n] = *str;
					n++;
					str++;
				}
				if (dst && *fmt == 's')
					dst[n] = '\0';
				if (!skip)
					count++;
				fmt++;
				continue;
			}

			{
				char digits[32];
				int n = 0, base = 10;
				bool is_signed = false;
				unsigned long long value;
				char *end;
				switch (*fmt) {
					case 'd': is_signed = true; break;
					case 'i': is_signed = true; base = 0; break;
					case 'u': break;
					case 'x': case 'X': base = 16; break;
					case 'o': base = 8; break;
					default:
						return count;
				}
				if (width < 0 || width > (int)sizeof(digits) - 1)
					width = sizeof(digits) - 1;
				while (*str != '\0' && n < width && !isspace(*str))
					digits[n++] = *str++;
				digits[n] = '\0';
				if (is_signed)
					value = simple_strtoll(digits, &end, base);
				else
					value = simple_strtoull(digits, &end, base);
				if (end == digits)
					break;
				str -= n - (end - digits);
				if (!skip) {
					void *p = va_arg(args, void *);
					switch (length) {
						case 'H': *(u8 *)p = value; break;
						case 'h': *(u16 *)p = value; break;
						case 'l': *(unsigned long *)p = value; break;
						case 'L': *(unsigned long long *)p = value; break;
						default: *(unsigned int *)p = value; break;
					}
					count++;
				}
				fmt++;
			}
		}
	}
	return count;
}


int
sscanf(const char *buf, const char *fmt, ...)
{
	va_list args;
	int count;
	va_start(args, fmt);
	count = vsscanf(buf, fmt, args);
	va_end(args);
	return count;
}
