#include <algorithm>

#include "GridColorFunction.hh"

namespace Amanzi {

int GridColorFunction::operator()(const double* x) const
{
  int offset = 0;
  for (int k = dim_-1; k >= 0; --k) {
    int i = (int) ((x[k] - x0_[k])/dx_[k]);
    i = std::min(i, count_[k] - 1);
    i = std::max(i, 0);
    offset = i + count_[k]*offset; // offset = 0 on first pass
  }
  return array_[offset];
}

} // namespace Amanzi
