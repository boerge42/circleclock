// ********************************************************************************
// ************************** Sonne ***********************************************
// inspired by: https://www.astronomie.info/zeitgleichung/

#include "astro.h"

float _rad = PI/180.0;
float _h = -(50.0/60.0)*_rad;
float _b = BREITE * _rad;

// ********************************************************************************
float sonnendeklination (float T)
{
  return (0.409526325277017 * sin (0.0169060504029192 * (T - 80.0856919827619)));
}

// ********************************************************************************
float zeitdifferenz (float Deklination)
{
  return (12.0*acos((sin(_h)-sin(_b)*sin(Deklination))/(cos(_b)*cos(Deklination)))/PI);
}

// ********************************************************************************
float zeitgleichung(float T)
{
  return (-0.170869921174742*sin(0.0336997028793971 * T + 0.465419984181394) - 0.129890681040717*sin(0.0178674832556871 *T - 0.167936777524864));
}

// ********************************************************************************
float _aufgang(float T)
{
  return (12 - zeitdifferenz(sonnendeklination(T)) - zeitgleichung(T));
}

// ********************************************************************************
float _untergang(float T)
{
  return (12 + zeitdifferenz(sonnendeklination(T)) - zeitgleichung(T));
}

// ********************************************************************************
float sunrise(float T, boolean isDST)
{
  int _zone;
  if (isDST)
    _zone = 2;
  else
    _zone = 1;
  return (_aufgang(T) - LAENGE / 15.0 + _zone);  
}

// ********************************************************************************
float sunset(float T, boolean isDST)
{
  int _zone;
  if (isDST)
    _zone = 2;
  else
    _zone = 1;
  return (_untergang(T) - LAENGE / 15.0 + _zone);
}
