FlexiCAS: A ***Flexi***ble ***C***ache ***A***rchitectural ***S***imulator
-------------------------------

GPL licensed.

Copyright (c) 2023-2024 Wei Song <[wsong83@gmail.com](mailto:wsong83@gmail.com)> at the Institute of
Information Engineering, Chinese Academy of Sciences.

#### Authors:
* [Wei Song](mailto:wsong83@gmail.com) (SKLOIS, Institute of Information Engineering, Chinese Academy of Sciences)
* [Jinchi Han](mailto:hanjinchi@iie.ac.cn) (SKLOIS, Institute of Information Engineering, Chinese Academy of Sciences)
* [Zhidong Wang](mailto:wangzhidong@iie.ac.cn) (SKLOIS, Institute of Information Engineering, Chinese Academy of Sciences)

## Features

* A pure C++ (std c++17) implementation of a modular cache architecture.
* Modular support for different index function, replacement policy, and slice mapping function.
* Modular support for complex cache array structure, such as separated metadata and data arrays.
* Modular support for different coherence protocols (MI/MSI/MESI), inclusing broadcast and directory.
* Modular support for inclusive/exclusive cache hierarchy.
* On-demand hooking up with user defined performance monitors.
* On-demand estimation of cache access delay (behavoral, not cycle accurate).

### Note

The project is under active developing. Please raise **issues** or **pull requests** for any sugguestion or fix.

This is an overhual of a previous in-house implemented simulator, namely [**cache-model**](https://github.com/comparch-security/cache-model).

## Usage

Right now, see the regression test cases in `regression/*.cpp` and try to run `make regression`.
