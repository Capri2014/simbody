/* Portions copyright (c) 2006-7 Stanford University and Michael Sherman.
 * Contributors:
 * 
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including 
 * without limitation the rights to use, copy, modify, merge, publish, 
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject
 * to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included 
 * in all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS, CONTRIBUTORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */


/**@file
 *
 * Private implementation of HuntCrossleyContact Subsystem.
 */

#include "SimTKsimbody.h"
#include "simbody/internal/ForceSubsystem.h"
#include "simbody/internal/HuntCrossleyContact.h"
#include "simbody/internal/MultibodySystem.h"

#include "ForceSubsystemRep.h"

namespace SimTK {

class HuntCrossleyContactRep : public ForceSubsystemRep {

    // Private class definitions for state entries.
    // Note that we take the 2/3 power of the plane-strain modulus
    // and store that as "stiffness" here; see comments on the
    // client-side class for why.

    struct SphereParameters {
        SphereParameters() { 
            center.setToNaN();
            radius = stiffness = dissipation = CNT<Real>::getNaN();
        }

        SphereParameters(MobilizedBodyId b, const Vec3& ctr,
                         const Real& r, const Real& k, const Real& c) 
          : body(b), center(ctr), radius(r), stiffness(std::pow(k,2./3.)), dissipation(c) { 
            assert(b.isValid());
            assert(radius > 0 && stiffness >= 0 && dissipation >= 0);
        }

        MobilizedBodyId body;
        Vec3 center;    // in body frame
        Real radius, stiffness, dissipation;    // r,k,c from H&C
    };

    struct HalfspaceParameters {
        HalfspaceParameters() { 
            height = stiffness = dissipation = CNT<Real>::getNaN();
        }

        HalfspaceParameters(MobilizedBodyId b, const UnitVec3& n,
                            const Real& h, const Real& k, const Real& c) 
          : body(b), normal(n), height(h), stiffness(std::pow(k,2./3.)), dissipation(c) { 
            assert(b.isValid());
            assert(stiffness >= 0 && dissipation >= 0);
        }

        MobilizedBodyId body;
        UnitVec3 normal;    // in body frame
        Real height, stiffness, dissipation;
    };

    struct Parameters {
        Parameters() : enabled(true) { }
        bool enabled;
        std::vector<SphereParameters>    spheres;
        std::vector<HalfspaceParameters> halfSpaces;
    };


public:
    HuntCrossleyContactRep()
     : ForceSubsystemRep("HuntCrossleyContact", "0.0.1"), 
       instanceVarsIndex(-1)
    {
    }
    int addSphere(MobilizedBodyId body, const Vec3& center,
                  const Real& radius,
                  const Real& stiffness,
                  const Real& dissipation) 
    {
        assert(body.isValid() && radius > 0 && stiffness >= 0 && dissipation >= 0);

        invalidateSubsystemTopologyCache(); // this is a topological change

        defaultParameters.spheres.push_back(
            SphereParameters(body,center,radius,stiffness,dissipation));
        return (int)defaultParameters.spheres.size() - 1;    
    }

    int addHalfSpace(MobilizedBodyId body, const UnitVec3& normal,
                     const Real& height,
                     const Real& stiffness,
                     const Real& dissipation)
    {
        assert(body.isValid() && stiffness >= 0 && dissipation >= 0);

        invalidateSubsystemTopologyCache(); // this is a topological change

        defaultParameters.halfSpaces.push_back(
            HalfspaceParameters(body,normal,height,stiffness,dissipation));
        return (int)defaultParameters.halfSpaces.size() - 1;
    }

    void realizeSubsystemTopologyImpl(State& s) const {
        instanceVarsIndex = s.allocateDiscreteVariable(getMySubsystemId(), Stage::Instance, 
            new Value<Parameters>(defaultParameters));
    }

    void realizeSubsystemModelImpl(State& s) const {
        // Sorry, no choices available at the moment.
    }

    void realizeSubsystemInstanceImpl(const State& s) const {
        // Nothing to compute here.
    }

    void realizeSubsystemTimeImpl(const State& s) const {
        // Nothing to compute here.
    }

    void realizeSubsystemPositionImpl(const State& s) const {
        // Nothing to compute here.
    }

    void realizeSubsystemVelocityImpl(const State& s) const {
        // Nothing to compute here.
    }

    // Cost of contact processing here (in flops):
    //      30*(ns*ns)/2 + 7*(ns*nhg) + 28*(ns*nhm) -- to determine whether there is contact
    //    + 156 * (number of actual contacts)
    // where ns==# spheres, nhg==# half spaces on ground, nhm==#half spaces on moving bodies. 
    // It doesn't take many objects before that first term is very expensive.
    // TODO: contact test can be made O(n) by calculating neighborhoods, e.g.

    void realizeSubsystemDynamicsImpl(const State& s) const;

    void realizeSubsystemAccelerationImpl(const State& s) const {
        // Nothing to compute here.
    }

    void realizeSubsystemReportImpl(const State& s) const {
        // Nothing to compute here.
    }

    HuntCrossleyContactRep* cloneSubsystemRep() const {return new HuntCrossleyContactRep(*this);}

private:
        // TOPOLOGY "STATE" VARIABLES

    Parameters defaultParameters;

        // TOPOLOGY "CACHE" VARIABLES

    // This must be filled in during realizeTopology and treated
    // as const thereafter. These are garbage unless built=true.
    mutable int instanceVarsIndex;

    const Parameters& getParameters(const State& s) const {
        assert(subsystemTopologyHasBeenRealized());
        return Value<Parameters>::downcast(
            getDiscreteVariable(s,instanceVarsIndex)).get();
    }
    Parameters& updParameters(State& s) const {
        assert(subsystemTopologyHasBeenRealized());
        return Value<Parameters>::downcast(
            updDiscreteVariable(s,instanceVarsIndex)).upd();
    }

    // We have determined that contact is occurring. The *undeformed* contact points and the
    // contact normal along which they lie have been determined. This method calculates and
    // applies the force to each body at the *deformed* contact point, accounting for the
    // different material properties. (cost ~144 flops)
    void processContact(const Real& R, // relative radius of curvature
                        const Real& k1, const Real& c1, const Transform& X_GB1, const SpatialVec& V_GB1,
                        const Real& k2, const Real& c2, const Transform& X_GB2, const SpatialVec& V_GB2, 
                        const Vec3& undefContactPt1_G, const Vec3& undefContactPt2_G,
                        const UnitVec3& contactNormal_G,    // points from body2 to body1
                        Real& pe, SpatialVec& force1, SpatialVec& force2) const;

    friend std::ostream& operator<<(std::ostream& o, 
                         const HuntCrossleyContactRep::Parameters&); 
};
// Useless, but required by Value<T>.
std::ostream& operator<<(std::ostream& o, 
                         const HuntCrossleyContactRep::Parameters&) 
{assert(false);return o;}



    /////////////////////////
    // HuntCrossleyContact //
    /////////////////////////

/*static*/ bool 
HuntCrossleyContact::isInstanceOf(const ForceSubsystem& s) {
    return HuntCrossleyContactRep::isA(s.getRep());
}
/*static*/ const HuntCrossleyContact&
HuntCrossleyContact::downcast(const ForceSubsystem& s) {
    assert(isInstanceOf(s));
    return reinterpret_cast<const HuntCrossleyContact&>(s);
}
/*static*/ HuntCrossleyContact&
HuntCrossleyContact::updDowncast(ForceSubsystem& s) {
    assert(isInstanceOf(s));
    return reinterpret_cast<HuntCrossleyContact&>(s);
}

const HuntCrossleyContactRep& 
HuntCrossleyContact::getRep() const {
    return dynamic_cast<const HuntCrossleyContactRep&>(*rep);
}
HuntCrossleyContactRep&       
HuntCrossleyContact::updRep() {
    return dynamic_cast<HuntCrossleyContactRep&>(*rep);
}

// Create Subsystem but don't associate it with any System. This isn't much use except
// for making std::vector's, which require a default constructor to be available.
HuntCrossleyContact::HuntCrossleyContact()
  : ForceSubsystem()
{
    rep = new HuntCrossleyContactRep();
    rep->setMyHandle(*this);
}

HuntCrossleyContact::HuntCrossleyContact(MultibodySystem& mbs)
  : ForceSubsystem()
{
    rep = new HuntCrossleyContactRep();
    rep->setMyHandle(*this);
    mbs.addForceSubsystem(*this); // steal ownership
}

int HuntCrossleyContact::addSphere(MobilizedBodyId body, const Vec3& center,
              const Real& radius,
              const Real& stiffness,
              const Real& dissipation)
{
    return updRep().addSphere(body,center,radius,stiffness,dissipation);
}

int HuntCrossleyContact::addHalfSpace(MobilizedBodyId body, const UnitVec3& normal,
                 const Real& height,
                 const Real& stiffness,
                 const Real& dissipation)
{
    return updRep().addHalfSpace(body,normal,height,stiffness,dissipation);
}



    ////////////////////////////
    // HuntCrossleyContactRep //
    ////////////////////////////

// Cost of contact processing here (in flops):
//      30*(ns*ns)/2 + 7*(ns*nhg) + 28*(ns*nhm) -- to determine whether there is contact
//    + 210 * (#sphere/sphere contacts) + 156 * (#sphere/halfspace contacts)
// where ns==# spheres, nhg==# half spaces on ground, nhm==#half spaces on moving bodies. 
// It doesn't take many spheres before that first term is very expensive.
// TODO: contact test can be made O(n) by calculating neighborhoods, e.g.

void HuntCrossleyContactRep::realizeSubsystemDynamicsImpl(const State& s) const 
{
    const Parameters& p = getParameters(s);
    if (!p.enabled) return;

    const MultibodySystem& mbs    = getMultibodySystem(); // my owner
    const MatterSubsystem& matter = mbs.getMatterSubsystem();

    // Get access to system-global cache entries.
    Real&                  pe              = mbs.updPotentialEnergy(s, Stage::Dynamics);
    Vector_<SpatialVec>&   rigidBodyForces = mbs.updRigidBodyForces(s, Stage::Dynamics);

    for (int s1=0; s1 < (int)p.spheres.size(); ++s1) {
        const SphereParameters& sphere1 = p.spheres[s1];
        const Transform&  X_GB1     = matter.getBodyTransform(s,sphere1.body);
        const SpatialVec& V_GB1     = matter.getBodyVelocity(s,sphere1.body); // in G
        const Real        r1        = sphere1.radius;
        const Vec3        center1_G = X_GB1*sphere1.center;

        for (int s2=s1+1; s2 < (int)p.spheres.size(); ++s2) {
            const SphereParameters& sphere2 = p.spheres[s2];
            if (sphere2.body == sphere1.body) continue;
            const Transform&  X_GB2     = matter.getBodyTransform(s,sphere2.body);
            const SpatialVec& V_GB2     = matter.getBodyVelocity(s,sphere2.body);
            const Real        r2        = sphere2.radius;
            const Vec3        center2_G = X_GB2*sphere2.center; // 18 flops

            const Vec3 c2c1_G    = center1_G - center2_G;   // points at sphere1 (3 flops)
            const Real dsq       = c2c1_G.normSqr();        // 5 flops
            if (dsq >= (r1+r2)*(r1+r2)) continue;           // 4 flops

            // There is a collision
            const Real     d = std::sqrt(dsq);       // distance between the centers (~30 flops)
            const UnitVec3 normal_G(c2c1_G/d, true); // direction from c2 center to c1 center (~12 flops)

            processContact((r1*r2)/(r1+r2), // relative curvature, ~12 flops
                           sphere1.stiffness, sphere1.dissipation, X_GB1, V_GB1,
                           sphere2.stiffness, sphere2.dissipation, X_GB2, V_GB2,
                           center1_G - r1 * normal_G, // undeformed contact point for sphere1 (6 flops)
                           center2_G + r2 * normal_G, // undeformed contact point for sphere2 (6 flops)
                           normal_G,
                           pe, rigidBodyForces[sphere1.body], rigidBodyForces[sphere2.body]);
        }

        // half spaces
        for (int h=0; h < (int)p.halfSpaces.size(); ++h) {
            const HalfspaceParameters& halfSpace = p.halfSpaces[h];
            if (halfSpace.body == sphere1.body) continue;

            // Quick escape in the common case where the half space is on ground (7 flops)
            if (halfSpace.body==0) {
                const UnitVec3& normal_G = halfSpace.normal;
                const Real      hc1_G    = ~center1_G*normal_G; // ht of center over ground
                if  (hc1_G - halfSpace.height < r1) {
                    // Collision of sphere1 with a half space on ground.
                    processContact(r1, // relative curvature is just r1
                        sphere1.stiffness, sphere1.dissipation, X_GB1, V_GB1,
                        halfSpace.stiffness, halfSpace.dissipation, Transform(), SpatialVec(Vec3(0)),
                        center1_G - (r1*normal_G), // undeformed contact point for sphere (6 flops)
                        halfSpace.height*normal_G, //            "             for halfSpace (3 flops)
                        normal_G, pe, rigidBodyForces[sphere1.body], rigidBodyForces[halfSpace.body]);
                }
                continue;
            }

            // Half space is not on ground.

            const Transform&  X_GB2    = matter.getBodyTransform(s,halfSpace.body);
            const SpatialVec& V_GB2    = matter.getBodyVelocity(s,halfSpace.body);
            const UnitVec3    normal_G = X_GB2.R()*halfSpace.normal;    // 15 flops

            // Find the heights of the half space surface and sphere center measured 
            // along the contact normal from the ground origin. Then we can get the
            // height of the sphere center over the half space surface.
            const Real h_G   = ~X_GB2.T()*normal_G + halfSpace.height; // 6 flops
            const Real hc1_G = ~center1_G*normal_G;                    // 5 flops
            const Real d = hc1_G - h_G;                                // 1 flop
            if (d >= r1) continue;                                     // 1 flop

           // There is a collision
            processContact(r1, // relative curvature is just r1
                           sphere1.stiffness, sphere1.dissipation, X_GB1, V_GB1,
                           halfSpace.stiffness, halfSpace.dissipation, X_GB2, V_GB2,
                           center1_G - r1 * normal_G, // undeformed contact point for sphere (6 flops)
                           center1_G -  d * normal_G, // undeformed contact point for halfSpace (6 flops)
                           normal_G,
                           pe, rigidBodyForces[sphere1.body], rigidBodyForces[halfSpace.body]);
        }
    }
}

// We have determined that contact is occurring. The *undeformed* contact points and the
// contact normal along which they lie have been determined. This method calculates and
// applies the force to each body at the *deformed* contact point, accounting for the
// different material properties. (cost ~144 flops)
//
// Note: k1 = E1^(2/3), E1 is plane-strain modulus of material 1, etc.
//       R = (r1*r2)/(r1+r2)   (sphere-sphere)
//       R = r1                (sphere-halfspace)
// We calculate:
//       s1 = k2/(k1+k2); s2 = 1-s1
//       k = k1*s1 
//       E = k^(3/2)
//       c = c1*s1 + c2*s2
// We need to calculate Hertz force fH
//      fH = 4/3 sqrt(R) E x^(3/2)
// We can do this all with one square root like this:
//      fH = 4/3 sqrt(R) k sqrt(k) x sqrt(x)
//         = 4/3 k x sqrt(R*k*x)
// Then the complete Hunt & Crossley force f is
//      fH(1 + 3/2 c v)  where v = xdot.
// We also want potential energy
//      pe = 2/5 * (4/3 sqrt(R) E)*x^(5/2) = 2/5 fH x
// TODO: If we want the patch radius a also (not currently needed) it is
//      a = sqrt(R*x)
// That would take another square root as coded but could be avoided
// by precalculating the combined material properties.
//
// Note that we don't apply the force or count potential energy if the
// calculated value is negative, meaning the bodies would be "sticking".
// That situation can only occur because an outside force is yanking
// the bodies apart.

void HuntCrossleyContactRep::processContact
   (const Real& R,
    const Real& k1, const Real& c1, const Transform& X_GB1, const SpatialVec& V_GB1,
    const Real& k2, const Real& c2, const Transform& X_GB2, const SpatialVec& V_GB2, 
    const Vec3& undefContactPt1_G, const Vec3& undefContactPt2_G,
    const UnitVec3& contactNormal_G,    // points from body2 to body1
    Real& pe, SpatialVec& force1, SpatialVec& force2) const
{
    const Real x = ~(undefContactPt2_G-undefContactPt1_G) * contactNormal_G; // 8 flops
    // Body 1 must be "above" body 2, meaning its undeformed contact point must be "below"
    // body 2's so that they are interpenetrating.
    assert(x > 0); 

    // Calculate the fraction of total squish x which will be undergone by body 1; body 2's
    // squish fraction will be 1-squish1.
    const Real squish1 = k2/(k1+k2);    // ~11 flops counting divide as ~10
    const Real squish2 = 1 - squish1;   // 1 flop

    // Now we can find the real contact point, which is a little up the normal from pt1
    const Vec3 contactPt_G = undefContactPt1_G + (squish1*x)*contactNormal_G; // 7 flops

    // Find the body stations coincident with the contact point so that we can calculate
    // their velocities.
    const Vec3 contactPt1_G = contactPt_G - X_GB1.T();                  // 3 flops
    const Vec3 contactPt2_G = contactPt_G - X_GB2.T();                  // 3 flops
    const Vec3 vContactPt1_G = V_GB1[1] + V_GB1[0] % contactPt1_G;      // 12 flops
    const Vec3 vContactPt2_G = V_GB2[1] + V_GB2[0] % contactPt2_G;      // 12 flops

    const Real v = ~(vContactPt2_G-vContactPt1_G)*contactNormal_G; // dx/dt (8 flops)

    const Real k=k1*squish1; // = k2*squish2   1 flop
    const Real c=c1*squish1 + c2*squish2;   // 3 flops

    const Real fH = (4./3.) * k * x * std::sqrt(R*k*x); // ~35 flops
    const Real f  = fH * (1 + 1.5*c*v);                 // 4 flops

    // If the resulting force is negative, the multibody system is "yanking"
    // the objects apart so fast that the material can't undeform fast enough
    // to keep up. That means it can't apply any force and that the stored
    // potential energy will now be wasted. (I suppose it would be dissipated
    // internally as the body's surface oscillated around its undeformed shape.)
    if (f > 0) {    // 1 flop
        pe += (2./5.) * fH * x;                             // 3 flops
        const Vec3 fvec = f * contactNormal_G; // points towards body1 (3 flops)
        force1 += SpatialVec( contactPt1_G % fvec, fvec);   // 15 flops
        force2 -= SpatialVec( contactPt2_G % fvec, fvec);   // 15 flops
    }
}

} // namespace SimTK

