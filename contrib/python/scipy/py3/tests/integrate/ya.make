# Generated by devtools/yamaker.

PY3TEST()

SUBSCRIBER(g:python-contrib)

VERSION(1.11.4)

ORIGINAL_SOURCE(mirror://pypi/s/scipy/scipy-1.11.4.tar.gz)

SIZE(MEDIUM)

PEERDIR(
    contrib/python/scipy/py3/tests
)

NO_LINT()

SRCDIR(contrib/python/scipy/py3)

TEST_SRCS(
    scipy/integrate/_ivp/tests/__init__.py
    scipy/integrate/_ivp/tests/test_ivp.py
    scipy/integrate/_ivp/tests/test_rk.py
    scipy/integrate/tests/__init__.py
    scipy/integrate/tests/test__quad_vec.py
    scipy/integrate/tests/test_banded_ode_solvers.py
    scipy/integrate/tests/test_bvp.py
    scipy/integrate/tests/test_integrate.py
)

END()
