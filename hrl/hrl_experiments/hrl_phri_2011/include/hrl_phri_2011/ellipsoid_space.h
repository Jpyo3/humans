#ifndef ELLIPSOID_SPACE_H
#define ELLIPSOID_SPACE_H
#include <cmath>
#include <ros/ros.h>
#include <hrl_phri_2011/EllipsoidParams.h>

using namespace std;

#define SQ(x) ((x) * (x))
#define PI 3.14159265

struct Ellipsoid 
{
    double A, B, E, height;
    Ellipsoid() : A(1), B(1), height(1)
    {
        E = sqrt(fabs(SQ(A) - SQ(B))) / A;
    }
    
    Ellipsoid(double a, double b) : A(a), B(b), height(1) 
    {
        E = sqrt(fabs(SQ(A) - SQ(B))) / A;
    }

    Ellipsoid(const hrl_phri_2011::EllipsoidParams& ep) 
    {
        setParams(ep);
    }

    void setParams(const hrl_phri_2011::EllipsoidParams& ep) 
    {
        A = 1;
        E = ep.E;
        B = sqrt(1 - SQ(E));
        height = ep.height;
    }

    void cartToEllipsoidal(double x, double y, double z, double& lat, double& lon, double& height);
    void ellipsoidalToCart(double lat, double lon, double height, double& x, double& y, double& z);
    void mollweideProjection(double lat, double lon, double& x, double& y);
};

void Ellipsoid::cartToEllipsoidal(double x, double y, double z, double& lat, double& lon, double& height) {
    lon = atan2(y, x);
    if(lon < 0) 
        lon += 2 * PI;
    double a = A * E;
    double p = sqrt(SQ(x) + SQ(y));
    lat = asin(sqrt((sqrt(SQ(SQ(z) - SQ(a) + SQ(p)) + SQ(2 * a * p)) / SQ(a) -
                     SQ(z / a) - SQ(p / a) + 1) / 2));
    if(z < 0)
        lat = PI - fabs(lat);
    double cosh_height = z / (a * cos(lat));
    height = log(cosh_height + sqrt(SQ(cosh_height) - 1));
}

void Ellipsoid::ellipsoidalToCart(double lat, double lon, double height, double& x, double& y, double& z) {
    double a = A * E;
    x = a * sinh(height) * sin(lat) * cos(lon);
    y = a * sinh(height) * sin(lat) * sin(lon);
    z = a * cosh(height) * cos(lat);
}

void Ellipsoid::mollweideProjection(double lat, double lon, double& x, double& y) {
    double a = A;
    double b = A * (1 - SQ(E)) / (PI * E) * (log((1 + E) / (1 - E)) + 2 * E / (1 - SQ(E)));
    double Sl = sin(lat);
    double k = PI * ( (log((1 + E * Sl) / (1 - E * Sl)) + 2 * E * Sl / (1 - SQ(E) * SQ(Sl))) /
                      (log((1 + E)      / (1 - E))      + 2 * E      / (1 - SQ(E))));
    double t = lat;
    double diff_val = 10000.0;
    while(fabs(diff_val) > 0.00001) {
        diff_val = - ( 2 * t + sin(2 * t) - k) / (2 + 2 * cos(2 * t));
        t += diff_val;
    }
    x = a * lon * cos(t);
    y = b * sin(t);
}
#endif // ELLIPSOID_SPACE_H
