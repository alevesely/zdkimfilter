#! /usr/bin/env python
"""
reading Perl pod whith Python :-O"""

import re
import regex
import cgi
from strrepl import StringReplace

linkfmt = regex.compile(r'^(?:([^\|]*)\|)?(?:([^\(/]+)\([158]\))?(?:/(\w+))?$')

#  = regex.compile (r'^ (?:([^\|]*)\|)?  (?:([^/]+)[158])?  (?:/(\w+))?  $')

def known_href(h):
	mo = linkfmt.match(h)
	if mo:
		target = None
		if mo.group(2) == None:
			target = ''
		elif mo.group(2) in ('zfilter_db', 'zdkimfilter.conf', 'zaggregate'):
			target = mo.group(2) + '.html'
		elif mo.group(2) == 'couriertcpd':
			target = 'http://www.courier-mta.org/' + mo.group(2) + '.html'

		if target != None:
			if mo.group(3):
				target += '#' + mo.group(3)

			return (target, mo.group(1))

		err = ' (not known)'
	else:
		err = ' (no match)'

	print 'missing link for', h, err
	return ('#', None)


def fmttag(char):
	return {'I': '<i>', 'B': '<b>', 'C': '<code>',
		'L': '<a>', 'E': '&', 'F': '<i>'}.get(char, '');

# see http://stackoverflow.com/questions/26385984/recursive-pattern-in-regex
podfmt = regex.compile(r'([IBCLEFSXZ])<((?:[^<>]+|(?R))*)>')

def pod2html(pod, prefix='<p>'):
	'''handle pod formatting'''
	html = StringReplace(pod)
	# for mo in reversed(list(podfmt.finditer(pod, overlapped = True))):
	for mo in podfmt.finditer(pod, overlapped = True):
		tag = fmttag(mo.group(1))
		if tag and mo.group(2):
			if tag == '<a>':
				target, repl = known_href(mo.group(2))
				stag = '<a href="%s">' % cgi.escape(target, quote=True)
				html.change(stag, mo.start(1), 2)
				if repl:
					prepl = pod2html(repl, '')
					html.change(prepl, mo.start(1), mo.end(2) - mo.start(2))
					html.change('</a>', mo.end(2), 1)
					break
			else:
				html.change(tag, mo.start(1), 2)

			if tag.startswith('<'):
				html.change('</' + tag[1:], mo.end(2), 1)
			else:
				assert tag == '&', "Unsupported pod " + mo.group(1) + '<'+ mo.group(2) +'>'
				html.change(';', mo.end(2), 1)
	return prefix + html.final()

item = re.compile(r'^=item\s*([^X]*)\s*X<([a-z_ ]+)>')
new_para = re.compile(r'^=([a-z]+[1-4]?)\s+([^X]*)')

def not_used_read_pod(fname):
	# global item, new_para
	f = open(fname, 'r')
	depth = 0
	tables = False
	fields = False
	class intro: pass
	class para_dd: pass
	para_dd.pending = False
	para_dd.para = ''
	def close_dd():
		if para_dd.pending:
			para_dd.para += '</dd>'
			para_dd.pending = False

	for pod_line in f:
		if pod_line.strip() == '':
			continue

		for line in f:
			if line.strip() == '':
				break
			pod_line += line

		mo = new_para.search(pod_line)
		if mo:
			verb = mo.group(1)
			if tables or intro.track:
				if verb == 'over':
					depth += 1
					para_dd.para += '<dl>'
					continue

				if verb == 'back' and depth > 0:
					depth -= 1
					close_dd()
					para_dd.para += '</dl>'
					continue

				if verb == 'item' and depth > 0 and mo.group(2):
					close_dd()
					para_dd.para += '\n<dt>' + pod2html(mo.group(2)) + '</dt><dd>'
					para_dd.pending = True
					continue

				if para_dd.para:
					close_dd()
					if tables:
						for table in tables:
							if fields:
								for field in fields:
									add_descr(table +'-'+ field, para_dd.para)
							else:
								add_descr(table, para_dd.para)
					else:
						intro.para += para_dd.para.strip()
					para_dd.para = ''

			if verb.startswith('head'):
				title = mo.group(2).strip()
				intro.para += '\n<h2>%s</h2>\n' % title.capitalize()
			else:
				intro.track = False

			mo = item.search(pod_line)
			if mo:
				fields = filter(bool, re.split(r'\W+', pod2fields(mo.group(1))))
				tables = filter(bool, re.split(r'\W+', mo.group(2)))
				# print fname + ' -> fields(', fields, ') and tables(', tables, ')'
				intro.track = False
			else:
				tables = False
				fields = False
		elif tables or intro.track:
			para_dd.para += pod2html(pod_line)
	f.close()
	return intro.para


if __name__ == '__main__':
	import sys
	if (len(sys.argv) > 1):
		for i in range(1, len(sys.argv), 1):
			print pod2html(sys.argv[i]), "\n"

