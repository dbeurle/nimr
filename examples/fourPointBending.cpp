
#include "mesh_reader.hpp"

#include <iostream>

int main()
{
    imr::mesh_reader reader("fourPointBending.msh",
                            imr::NodalOrdering::Global,
                            imr::IndexingBase::One,
                            imr::distributed::feti);

    reader.write();
    std::cout << "Done!" << std::endl;
}
