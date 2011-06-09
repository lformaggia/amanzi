/* -*-  mode: c++; c-default-style: "google"; indent-tabs-mode: nil -*- */
#ifndef AMANZI_CHEMISTRY_BLOCK_HH_
#define AMANZI_CHEMISTRY_BLOCK_HH_

// Boost may provide us with a more optimal matrix implementation - Glenn

class Block {
  
 public:
  Block();
  Block(int);
  virtual ~Block();

  int getSize(void) const { return this->size; };
  double **getValues(void) const { return this->A; };
  double GetValue(const int& i, const int& j) const { return this->A[i][j]; };

  double getRowAbsMax(int);

  void setValue(int, int, double);
  void setValues(double **);
  void setValues(Block *);
  void setValues(int, int, Block *);
  void setValues(double **, double scale);
  void setValues(Block *, double scale);
  void setValues(int, int, Block *, double scale);

  void addValue(int, int, double);
  void addValues(double **);
  void addValues(Block *);
  void addValues(int, int, Block *);
  void addValues(double **, double scale);
  void addValues(Block *, double scale);
  void addValues(int, int, Block *, double scale);

  void scaleRow(int, double);
  void scaleColumn(int, double);
  void scale(double);

  void zero(void);
  void setDiagonal(double d);

  void print(void);

  
 private:

  int size;
  double **A;

};

#endif // AMANZI_CHEMISTRY_BLOCK_HH_

