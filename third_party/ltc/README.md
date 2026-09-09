# GGX linearly transformed cosines

The 64 × 64 inverse-matrix and amplitude/Fresnel tables are from
[selfshadow/ltc_code](https://github.com/selfshadow/ltc_code), commit
`31e5e96b54f98f33098f8503003119ba2231a1c6`, `fit/results/ltc.js`.
`ltc_tables.inc` preserves the published numbers as C++ binary32 literals;
matrix rows precede amplitude rows. The unused amplitude z/w lanes are retained.
The data is compiled into the renderer and never read from disk during a frame.

Reference: Eric Heitz, Jonathan Dupuy, Stephen Hill and David Neubelt,
*Real-Time Polygonal-Light Shading with Linearly Transformed Cosines*,
ACM Transactions on Graphics 35(4), SIGGRAPH 2016.
[Project and paper](https://eheitzresearch.wordpress.com/415-2/).
See LICENSE for redistribution terms; Windows packages include that license.
