# While perhaps not required in practice, we should have the ability to
# OSR from inside a list comprehension.
# statcheck: stats['OSR exits'] in (1, 2)

def p(i):
    print i
for i in xrange(100000):
    pass
# [i for i in xrange(100000) if i % 100 == 0]
