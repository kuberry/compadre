# Configuration file for the Sphinx documentation builder.
#
# This file only contains a selection of the most common options. For a full
# list see the documentation:
# https://www.sphinx-doc.org/en/master/usage/configuration.html

# -- Path setup --------------------------------------------------------------

# If extensions (or modules to document with autodoc) are in another directory,
# add these directories to sys.path here. If the directory is relative to the
# documentation root, use os.path.abspath to make it absolute, like shown here.
#
# import os
# import sys
# sys.path.insert(0, os.path.abspath('.'))
import sys
import os
#tls_cacert = "/Users/pakuber/sandia_root_ca.cer"
tls_verify = False


# -- Project information -----------------------------------------------------

project = 'PyCompadre'
copyright = '2021, National Technology & Engineering Solutions of Sandia, LLC (NTESS)'
author = 'Paul Kuberry'

# The full version, including alpha/beta/rc tags
release = '1.3.9'

# -- General configuration ---------------------------------------------------

# Add any Sphinx extension module names here, as strings. They can be
# extensions coming with Sphinx (named 'sphinx.ext.*') or your custom
# ones.
extensions = [
    'sphinx.ext.autodoc',
    'sphinx.ext.intersphinx',
    'sphinx.ext.autosummary',
    'sphinx.ext.napoleon',
]


# Add any paths that contain templates here, relative to this directory.
templates_path = ['_templates']

# List of patterns, relative to source directory, that match files and
# directories to ignore when looking for source files.
# This pattern also affects html_static_path and html_extra_path.
exclude_patterns = ['pybind11/*']


# -- Options for HTML output -------------------------------------------------

# The theme to use for HTML and HTML Help pages.  See the documentation for
# a list of builtin themes.
#
html_theme = 'sphinx_rtd_theme'

# Add any paths that contain custom static files (such as style sheets) here,
# relative to this directory. They are copied after the builtin static files,
# so a file named "default.css" will overwrite the builtin "default.css".
html_static_path = ['_static']

#latex_documents = [
#    (master_doc, 'python_example.tex', u'python_example Documentation',
#     u'Sylvain Corlay', 'manual'),
#]
#pygments_style = 'sphinx'
#man_pages = [
#    (master_doc, 'python_example', u'python_example Documentation',
#     [author], 1)
#]
#texinfo_documents = [
#    (master_doc, 'python_example', u'python_example Documentation',
#     author, 'python_example', 'One line description of project.',
#     'Miscellaneous'),
#]
intersphinx_mapping = {'https://docs.python.org/': None}
