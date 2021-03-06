/*
  Operators

  Copyright 2010-201x held jointly by LANS/LANL, LBNL, and PNNL.
  Amanzi is released under the three-clause BSD License.
  The terms of use and "as is" disclaimer for this license are
  provided in the top-level COPYRIGHT file.

  Authors: Konstantin Lipnikov (lipnikov@lanl.gov)
           Ethan Coon (ecoon@lanl.gov)
*/

#include "DenseMatrix.hh"
#include "Op_Cell_FaceCell.hh"

#include "SuperMap.hh"
#include "GraphFE.hh"
#include "MatrixFE.hh"
#include "Operator_ConsistentFace.hh"

/* ******************************************************************
Operator whose unknowns are CELL

See Operator_ConsistentFace.hh for more detail.
****************************************************************** */

namespace Amanzi {
namespace Operators {


/* ******************************************************************
* Visit methods for Apply.
* Apply the local matrices directly as schema is a subset of 
* assembled schema.
****************************************************************** */
int Operator_ConsistentFace::ApplyMatrixFreeOp(const Op_Cell_FaceCell& op,
                                     const CompositeVector& X, CompositeVector& Y) const
{
  ASSERT(op.matrices.size() == ncells_owned);

  Y.PutScalarGhosted(0.);
  X.ScatterMasterToGhosted();
  const Epetra_MultiVector& Xf = *X.ViewComponent("face", true);

  {
    Epetra_MultiVector& Yf = *Y.ViewComponent("face", true);

    AmanziMesh::Entity_ID_List faces;
    for (int c=0; c!=ncells_owned; ++c) {
      mesh_->cell_get_faces(c, &faces);
      int nfaces = faces.size();

      WhetStone::DenseVector v(nfaces), av(nfaces); av.clear();
      for (int n=0; n!=nfaces; ++n) {
        v(n) = Xf[0][faces[n]];
      }

      const WhetStone::DenseMatrix& Acell = op.matrices[c];
      // must do multiply manually because Acell is not nfaces x nfaces
      for (int n=0; n!=nfaces; ++n)
        for (int m=0; m!=nfaces; ++m)
          av(m) += Acell(m,n) * v(n);

      for (int n=0; n!=nfaces; ++n) {
        Yf[0][faces[n]] += av(n);
        ASSERT(std::abs(av(n)) < 1.e20);
      }
    } 
  }
  Y.GatherGhostedToMaster("face", Add);
  return 0;
}


/* ******************************************************************
* Visit methods for symbolic assemble.
* Insert the diagonal on cells
****************************************************************** */
void Operator_ConsistentFace::SymbolicAssembleMatrixOp(const Op_Cell_FaceCell& op,
                                             const SuperMap& map, GraphFE& graph,
                                             int my_block_row, int my_block_col) const
{
  int lid_r[OPERATOR_MAX_FACES];
  int lid_c[OPERATOR_MAX_FACES];

  // ELEMENT: cell, DOFS: cell and face
  const std::vector<int>& face_row_inds = map.GhostIndices("face", my_block_row);
  const std::vector<int>& face_col_inds = map.GhostIndices("face", my_block_col);

  int ierr(0);
  AmanziMesh::Entity_ID_List faces;
  for (int c=0; c!=ncells_owned; ++c) {
    mesh_->cell_get_faces(c, &faces);
    int nfaces = faces.size();

    for (int n=0; n!=nfaces; ++n) {
      lid_r[n] = face_row_inds[faces[n]];
      lid_c[n] = face_col_inds[faces[n]];
    }
    ierr |= graph.InsertMyIndices(nfaces, lid_r, nfaces, lid_c);
  }
  ASSERT(!ierr);
}


/* ******************************************************************
* Visit methods for assemble
* Insert each cells neighboring cells.
****************************************************************** */
void Operator_ConsistentFace::AssembleMatrixOp(const Op_Cell_FaceCell& op,
                                     const SuperMap& map, MatrixFE& mat,
                                     int my_block_row, int my_block_col) const
{
  ASSERT(op.matrices.size() == ncells_owned);

  int lid_r[OPERATOR_MAX_FACES];
  int lid_c[OPERATOR_MAX_FACES];
  double vals[OPERATOR_MAX_FACES];

  // ELEMENT: cell, DOFS: face and cell
  const std::vector<int>& face_row_inds = map.GhostIndices("face", my_block_row);
  const std::vector<int>& face_col_inds = map.GhostIndices("face", my_block_col);

  int ierr(0);
  AmanziMesh::Entity_ID_List faces;
  for (int c=0; c!=ncells_owned; ++c) {
    mesh_->cell_get_faces(c, &faces);
    
    int nfaces = faces.size();
    for (int n=0; n!=nfaces; ++n) {
      lid_r[n] = face_row_inds[faces[n]];
      lid_c[n] = face_col_inds[faces[n]];
    }

    for (int n=0; n!=nfaces; ++n) {
      for (int m=0; m!=nfaces; ++m) vals[m] = op.matrices[c](n,m);
      ierr |= mat.SumIntoMyValues(lid_r[n], nfaces, vals, lid_c);
    }
  }
  ASSERT(!ierr);
}

}  // namespace Operators
}  // namespace Amanzi

