# Third-party notices

Aurora is licensed under the Apache License, Version 2.0. The Apache
License applies only to Aurora's first-party material. Third-party
components retain their own copyright and license terms.

Aurora does not vendor the optional system and Python packages listed
below. Builds or installations that obtain them must preserve the license
materials shipped by those projects.

| Component | How Aurora uses it | License |
|---|---|---|
| [Wirehair](https://github.com/catid/wirehair), pinned at `067ca7cdb66aed424ec23f97557429bf791c6f0c` | Optional external FEC baseline obtained through CMake `FetchContent` or a user-supplied checkout | BSD 3-Clause |
| [libsodium](https://github.com/jedisct1/libsodium) | Optional system/user-supplied cryptographic library | ISC |
| [Dash](https://github.com/plotly/dash) | Optional Python dashboard dependency | MIT |
| [Plotly.py](https://github.com/plotly/plotly.py) | Optional Python dashboard dependency | MIT |

The repository also references these Git submodules. Their files are not
relicensed under Apache-2.0, and their checked-out license files control:

| Submodule | Pinned commit | License note |
|---|---|---|
| [Eigen](https://gitlab.com/libeigen/eigen) | `06f5cb4878f01cd5acbf7285df79f96ab2d68ac0` | Primarily MPL-2.0, with additional per-file third-party terms documented by Eigen |
| [libRaptorQ](https://github.com/LucaFulchir/libRaptorQ) | `a394b22af6e8fc742ce88eacf9af5343ecded9d7` | The upstream tree contains LGPL-3.0 and GPL-3.0 license materials; consult the license header of each file and the upstream license files before enabling the legacy integration |

This inventory is informational and does not replace any third-party license
text. When redistributing a build containing a third-party component, include
the corresponding upstream copyright and license notices.
