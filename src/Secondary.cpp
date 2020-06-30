/* 
   Created by: Maarten L. Hekkelman
   Date: woensdag 06 juni, 2018
*/

// Calculate DSSP-like secondary structure information

#include "cif++/Config.h"

#include <numeric>
#include <iomanip>
#include <thread>

#include <boost/algorithm/string.hpp>

#include "cif++/Structure.h"
#include "cif++/Secondary.h"

namespace ba = boost::algorithm;

// --------------------------------------------------------------------

namespace mmcif
{

struct Res;

enum ResidueType
{
	kUnknownResidue,
	
	//
	kAlanine,				// A	ala
	kArginine,				// R	arg
	kAsparagine,			// N	asn
	kAsparticAcid,			// D	asp
	kCysteine,				// C	cys
	kGlutamicAcid,			// E	glu
	kGlutamine,				// Q	gln
	kGlycine,				// G	gly
	kHistidine,				// H	his
	kIsoleucine,			// I	ile
	kLeucine,				// L	leu
	kLysine,				// K	lys
	kMethionine,			// M	met
	kPhenylalanine,			// F	phe
	kProline,				// P	pro
	kSerine,				// S	ser
	kThreonine,				// T	thr
	kTryptophan,			// W	trp
	kTyrosine,				// Y	tyr
	kValine,				// V	val
	
	kResidueTypeCount
};

struct ResidueInfo
{
	ResidueType		type;
	char			code;
	char			name[4];
};

const ResidueInfo kResidueInfo[] = {
	{ kUnknownResidue,	'X', "UNK" },
	{ kAlanine,			'A', "ALA" },
	{ kArginine,		'R', "ARG" },
	{ kAsparagine,		'N', "ASN" },
	{ kAsparticAcid,	'D', "ASP" },
	{ kCysteine,		'C', "CYS" },
	{ kGlutamicAcid,	'E', "GLU" },
	{ kGlutamine,		'Q', "GLN" },
	{ kGlycine,			'G', "GLY" },
	{ kHistidine,		'H', "HIS" },
	{ kIsoleucine,		'I', "ILE" },
	{ kLeucine,			'L', "LEU" },
	{ kLysine,			'K', "LYS" },
	{ kMethionine,		'M', "MET" },
	{ kPhenylalanine,	'F', "PHE" },
	{ kProline,			'P', "PRO" },
	{ kSerine,			'S', "SER" },
	{ kThreonine,		'T', "THR" },
	{ kTryptophan,		'W', "TRP" },
	{ kTyrosine,		'Y', "TYR" },
	{ kValine,			'V', "VAL" }
};

ResidueType MapResidue(std::string inName)
{
	ba::trim(inName);

	ResidueType result = kUnknownResidue;
	
	for (auto& ri: kResidueInfo)
	{
		if (inName == ri.name)
		{
			result = ri.type;
			break;
		}
	}
	
	return result;
}

struct HBond
{
	Res*	residue;
	double	energy;
};

enum BridgeType
{
	btNoBridge, btParallel, btAntiParallel
};

struct Bridge
{
	BridgeType				type;
	uint32_t				sheet, ladder;
	std::set<Bridge*>		link;
	std::deque<uint32_t>	i, j;
	std::string				chainI, chainJ;
	
	bool			operator<(const Bridge& b) const		{ return chainI < b.chainI or (chainI == b.chainI and i.front() < b.i.front()); }
};

struct BridgeParner
{
	Res*		residue;
	uint32_t	ladder;
	bool		parallel;
};

// --------------------------------------------------------------------

const float
	// kSSBridgeDistance = 3.0,
	kMinimalDistance = 0.5,
	kMinimalCADistance = 9.0,
	kMinHBondEnergy = -9.9,
	kMaxHBondEnergy = -0.5,
	kCouplingConstant = -27.888,	//	= -332 * 0.42 * 0.2
	kMaxPeptideBondLength = 2.5;

const float
	kRadiusN = 1.65,
	kRadiusCA = 1.87,
	kRadiusC = 1.76,
	kRadiusO = 1.4,
	kRadiusSideAtom = 1.8,
	kRadiusWater = 1.4;

struct Res
{
	Res(const Monomer& m, int nr)
		: mM(m), mNumber(nr)
		, mType(MapResidue(m.compoundID()))
	{
		// update the box containing all atoms
		mBox[0].mX = mBox[0].mY = mBox[0].mZ =  std::numeric_limits<double>::max();
		mBox[1].mX = mBox[1].mY = mBox[1].mZ = -std::numeric_limits<double>::max();

		for (auto& a: mM.atoms())
		{
			if (a.labelAtomID() == "CA")
			{
				mCAlpha = a.location();
				ExtendBox(mCAlpha, kRadiusCA + 2 * kRadiusWater);
			}
			else if (a.labelAtomID() == "C")
			{
				mC = a.location();
				ExtendBox(mC, kRadiusC + 2 * kRadiusWater);
			}
			else if (a.labelAtomID() == "N")
			{
				mN = a.location();
				ExtendBox(mN, kRadiusN + 2 * kRadiusWater);
			}
			else if (a.labelAtomID() == "O")
			{
				mO = a.location();
				ExtendBox(mO, kRadiusO + 2 * kRadiusWater);
			}
			else
			{
				mSideChain.push_back(a.location());
				ExtendBox(a.location(), kRadiusSideAtom + 2 * kRadiusWater);
			}
		}

		mRadius = mBox[1].mX - mBox[0].mX;
		if (mRadius < mBox[1].mY - mBox[0].mY)
			mRadius = mBox[1].mY - mBox[0].mY;
		if (mRadius < mBox[1].mZ - mBox[0].mZ)
			mRadius = mBox[1].mZ - mBox[0].mZ;

		mCenter.mX = (mBox[0].mX + mBox[1].mX) / 2;
		mCenter.mY = (mBox[0].mY + mBox[1].mY) / 2;
		mCenter.mZ = (mBox[0].mZ + mBox[1].mZ) / 2;
	}

	void assignHydrogen()
	{
		// assign the Hydrogen
		mH = mN;
		
		if (mType != kProline and mPrev != nullptr)
		{
			auto pc = mPrev->mC;
			auto po = mPrev->mO;
			
			double CODistance = Distance(pc, po);
			
			mH.mX += (pc.mX - po.mX) / CODistance; 
			mH.mY += (pc.mY - po.mY) / CODistance; 
			mH.mZ += (pc.mZ - po.mZ) / CODistance; 
		}
	}

	void SetSecondaryStructure(SecondaryStructureType inSS)	{ mSecondaryStructure = inSS; }
	SecondaryStructureType GetSecondaryStructure() const	{ return mSecondaryStructure; }
	
	void SetBetaPartner(uint32_t n, Res& inResidue, uint32_t inLadder, bool inParallel)
	{
		assert(n == 0 or n == 1);
		
		mBetaPartner[n].residue = &inResidue;
		mBetaPartner[n].ladder = inLadder;
		mBetaPartner[n].parallel = inParallel;
	}
	
	BridgeParner GetBetaPartner(uint32_t n) const
	{
		assert(n == 0 or n == 1);
		return mBetaPartner[n];
	}
						
	void SetSheet(uint32_t inSheet)						{ mSheet = inSheet; }
	uint32_t GetSheet() const							{ return mSheet; }
	
	bool IsBend() const									{ return mBend; }
	void SetBend(bool inBend)							{ mBend = inBend; }
	
	Helix GetHelixFlag(uint32_t inHelixStride) const
	{
		assert(inHelixStride == 3 or inHelixStride == 4 or inHelixStride == 5);
		return mHelixFlags[inHelixStride - 3];
	}

	bool IsHelixStart(uint32_t inHelixStride) const
	{
		assert(inHelixStride == 3 or inHelixStride == 4 or inHelixStride == 5);
		return mHelixFlags[inHelixStride - 3] == Helix::Start or mHelixFlags[inHelixStride - 3] == Helix::StartAndEnd;
	}

	void SetHelixFlag(uint32_t inHelixStride, Helix inHelixFlag)
	{
		assert(inHelixStride == 3 or inHelixStride == 4 or inHelixStride == 5);
		mHelixFlags[inHelixStride - 3] = inHelixFlag;
	}

	void SetSSBridgeNr(uint8_t inBridgeNr)
	{
		if (mType != kCysteine)
			throw std::runtime_error("Only cysteine residues can form sulphur bridges");
		mSSBridgeNr = inBridgeNr;
	}
	
	uint8_t GetSSBridgeNr() const
	{
		if (mType != kCysteine)
			throw std::runtime_error("Only cysteine residues can form sulphur bridges");
		return mSSBridgeNr;
	}

	double CalculateSurface(const std::vector<Res>& inResidues);
	double CalculateSurface(const Point& inAtom, float inRadius, const std::vector<Res*>& inNeighbours);

	bool AtomIntersectsBox(const Point& atom, float inRadius) const
	{
		return
			atom.mX + inRadius >= mBox[0].mX and
			atom.mX - inRadius <= mBox[1].mX and
			atom.mY + inRadius >= mBox[0].mY and
			atom.mY - inRadius <= mBox[1].mY and
			atom.mZ + inRadius >= mBox[0].mZ and
			atom.mZ - inRadius <= mBox[1].mZ;
	}

	void ExtendBox(const Point& atom, float inRadius)
	{
		if (mBox[0].mX > atom.mX - inRadius)
			mBox[0].mX = atom.mX - inRadius;
		if (mBox[0].mY > atom.mY - inRadius)
			mBox[0].mY = atom.mY - inRadius;
		if (mBox[0].mZ > atom.mZ - inRadius)
			mBox[0].mZ = atom.mZ - inRadius;
		if (mBox[1].mX < atom.mX + inRadius)
			mBox[1].mX = atom.mX + inRadius;
		if (mBox[1].mY < atom.mY + inRadius)
			mBox[1].mY = atom.mY + inRadius;
		if (mBox[1].mZ < atom.mZ + inRadius)
			mBox[1].mZ = atom.mZ + inRadius;
	}

	Res* mNext = nullptr;
	Res* mPrev = nullptr;

	const Monomer& mM;

	int mNumber;

	Point mCAlpha, mC, mN, mO, mH;
	Point mBox[2] = {};
	float mRadius;
	Point mCenter;
	std::vector<Point> mSideChain;
	double mAccessibility = 0;
	
	ResidueType mType;
	uint8_t mSSBridgeNr = 0;
	SecondaryStructureType mSecondaryStructure = ssLoop;
	HBond mHBondDonor[2] = {}, mHBondAcceptor[2] = {};
	BridgeParner mBetaPartner[2] = {};
	uint32_t mSheet = 0;
	Helix mHelixFlags[3] = { Helix::None, Helix::None, Helix::None };	//
	bool mBend = false;
};

// --------------------------------------------------------------------

class Accumulator
{
	public:

	struct candidate
	{
		Point	location;
		double	radius;
		double	distance;

		bool operator<(const candidate& rhs) const
				{ return distance < rhs.distance; }
	};

	void operator()(const Point& a, const Point& b, double d, double r)
	{
		double distance = DistanceSquared(a, b);

		d += kRadiusWater;
		r += kRadiusWater;

		double test = d + r;
		test *= test;

		if (distance < test and distance > 0.0001)
		{
			candidate c = { b - a, r * r, distance };

			m_x.push_back(c);
			push_heap(m_x.begin(), m_x.end());
		}
	}

	void sort()
	{
		sort_heap(m_x.begin(), m_x.end());
	}

	std::vector<candidate>  m_x;
};

// we use a fibonacci spheres to calculate the even distribution of the dots
class MSurfaceDots
{
  public:

	static MSurfaceDots&  Instance();

	size_t size() const							{ return mPoints.size(); }
	const Point& operator[](size_t inIx) const	{ return mPoints[inIx]; }
	double weight() const						{ return mWeight; }

  private:
	MSurfaceDots(int32_t inN);

	std::vector<Point> mPoints;
	double mWeight;
};

MSurfaceDots& MSurfaceDots::Instance()
{
	const int32_t kN = 200;

	static MSurfaceDots sInstance(kN);
	return sInstance;
}

MSurfaceDots::MSurfaceDots(int32_t N)
{
	auto P = 2 * N + 1;

	const double kGoldenRatio = (1 + sqrt(5.0)) / 2;

	mWeight = (4 * kPI) / P;

	for (auto i = -N; i <= N; ++i)
	{
		double lat = asin((2.0 * i) / P);
		double lon = fmod(i, kGoldenRatio) * 2 * kPI / kGoldenRatio;

		mPoints.emplace_back(sin(lon) * cos(lat), cos(lon) * cos(lat), sin(lat));
	}
}

double Res::CalculateSurface(const Point& inAtom, float inRadius, const std::vector<Res*>& inNeighbours)
{
	Accumulator accumulate;

	for (auto r: inNeighbours)
	{
		if (r->AtomIntersectsBox(inAtom, inRadius))
		{
			accumulate(inAtom, r->mN, inRadius, kRadiusN);
			accumulate(inAtom, r->mCAlpha, inRadius, kRadiusCA);
			accumulate(inAtom, r->mC, inRadius, kRadiusC);
			accumulate(inAtom, r->mO, inRadius, kRadiusO);

			for (auto& atom: r->mSideChain)
				accumulate(inAtom, atom, inRadius, kRadiusSideAtom);
		}
	}

	accumulate.sort();

	float radius = inRadius + kRadiusWater;
	double surface = 0;

	MSurfaceDots& surfaceDots = MSurfaceDots::Instance();

	for (size_t i = 0; i < surfaceDots.size(); ++i)
	{
		Point xx = surfaceDots[i] * radius;

		bool free = true;
		for (size_t k = 0; free and k < accumulate.m_x.size(); ++k)
			free = accumulate.m_x[k].radius < DistanceSquared(xx, accumulate.m_x[k].location);

		if (free)
			surface += surfaceDots.weight();
	}

	return surface * radius * radius;
}

double Res::CalculateSurface(const std::vector<Res>& inResidues)
{
	std::vector<Res*> neighbours;

	for (auto& r: inResidues)
	{
		Point center = r.mCenter;
		double radius = r.mRadius;

		if (Distance(mCenter, center) < mRadius + radius)
			neighbours.push_back(const_cast<Res*>(&r));
	}

	mAccessibility = CalculateSurface(mN, kRadiusN, neighbours) +
					 CalculateSurface(mCAlpha, kRadiusCA, neighbours) +
					 CalculateSurface(mC, kRadiusC, neighbours) +
					 CalculateSurface(mO, kRadiusO, neighbours);

	for (auto& atom: mSideChain)
		mAccessibility += CalculateSurface(atom, kRadiusSideAtom, neighbours);
	
	return mAccessibility;
}

void CalculateAccessibilities(std::vector<Res>& inResidues, DSSP_Statistics& stats)
{
	if (cif::VERBOSE)
		std::cerr << "Calculate accessibilities" << std::endl;

	stats.accessibleSurface = 0;

	for (auto& residue: inResidues)
		stats.accessibleSurface += residue.CalculateSurface(inResidues);
}

// --------------------------------------------------------------------
// TODO: use the angle to improve bond energy calculation.

double CalculateHBondEnergy(Res& inDonor, Res& inAcceptor)
{
	double result = 0;
	
	if (inDonor.mType != kProline)
	{
		double distanceHO = Distance(inDonor.mH, inAcceptor.mO);
		double distanceHC = Distance(inDonor.mH, inAcceptor.mC);
		double distanceNC = Distance(inDonor.mN, inAcceptor.mC);
		double distanceNO = Distance(inDonor.mN, inAcceptor.mO);
		
		if (distanceHO < kMinimalDistance or distanceHC < kMinimalDistance or distanceNC < kMinimalDistance or distanceNO < kMinimalDistance)
			result = kMinHBondEnergy;
		else
			result = kCouplingConstant / distanceHO - kCouplingConstant / distanceHC + kCouplingConstant / distanceNC - kCouplingConstant / distanceNO;

		// DSSP compatibility mode:
		result = round(result * 1000) / 1000;

		if (result < kMinHBondEnergy)
			result = kMinHBondEnergy;
	}

	// update donor
	if (result < inDonor.mHBondAcceptor[0].energy)
	{
		inDonor.mHBondAcceptor[1] = inDonor.mHBondAcceptor[0];
		inDonor.mHBondAcceptor[0].residue = &inAcceptor;
		inDonor.mHBondAcceptor[0].energy = result;
	}
	else if (result < inDonor.mHBondAcceptor[1].energy)
	{
		inDonor.mHBondAcceptor[1].residue = &inAcceptor;
		inDonor.mHBondAcceptor[1].energy = result;
	}		

	// and acceptor
	if (result < inAcceptor.mHBondDonor[0].energy)
	{
		inAcceptor.mHBondDonor[1] = inAcceptor.mHBondDonor[0];
		inAcceptor.mHBondDonor[0].residue = &inDonor;
		inAcceptor.mHBondDonor[0].energy = result;
	}
	else if (result < inAcceptor.mHBondDonor[1].energy)
	{
		inAcceptor.mHBondDonor[1].residue = &inDonor;
		inAcceptor.mHBondDonor[1].energy = result;
	}		
	
	return result;
}


// --------------------------------------------------------------------

void CalculateHBondEnergies(std::vector<Res>& inResidues)
{
	// Calculate the HBond energies
	for (uint32_t i = 0; i + 1 < inResidues.size(); ++i)
	{
		auto& ri = inResidues[i];
		
		for (uint32_t j = i + 1; j < inResidues.size(); ++j)
		{
			auto& rj = inResidues[j];
			
			if (Distance(ri.mCAlpha, rj.mCAlpha) < kMinimalCADistance)
			{
				CalculateHBondEnergy(ri, rj);
				if (j != i + 1)
					CalculateHBondEnergy(rj, ri);
			}
		}
	}
}

// --------------------------------------------------------------------

bool NoChainBreak(const Res* a, const Res* b)
{
	return a->mM.asymID() == b->mM.asymID();
}

bool NoChainBreak(const Res& a, const Res& b)
{
	return a.mM.asymID() == b.mM.asymID();
}

// --------------------------------------------------------------------

bool TestBond(const Res* a, const Res* b)
{
	return
		(a->mHBondAcceptor[0].residue == b and a->mHBondAcceptor[0].energy < kMaxHBondEnergy) or
		(a->mHBondAcceptor[1].residue == b and a->mHBondAcceptor[1].energy < kMaxHBondEnergy);
}

// --------------------------------------------------------------------

BridgeType TestBridge(const Res& r1, const Res& r2)
{										// I.	a	d	II.	a	d		parallel    
	auto a = r1.mPrev;					//		  \			  /
	auto b = &r1;						//		b	e		b	e
	auto c = r1.mNext;					// 		  /			  \                      ..
	auto d = r2.mPrev;					//		c	f		c	f
	auto e = &r2;						//
	auto f = r2.mNext;					// III.	a <- f	IV. a	  f		antiparallel
										//		                                   
	BridgeType result = btNoBridge;		//		b	 e      b <-> e                  
										//                                          
										//		c -> d		c     d
										
	if (a and c and NoChainBreak(a, c) and d and f and NoChainBreak(d, f))
	{
		if ((TestBond(c, e) and TestBond(e, a)) or (TestBond(f, b) and TestBond(b, d)))
			result = btParallel;
		else if ((TestBond(c, d) and TestBond(f, a)) or (TestBond(e, b) and TestBond(b, e)))
			result = btAntiParallel;
	}
	
	return result;
}

// --------------------------------------------------------------------
// return true if any of the residues in bridge a is identical to any of the residues in bridge b
bool Linked(const Bridge& a, const Bridge& b)
{
	return
		find_first_of(a.i.begin(), a.i.end(), b.i.begin(), b.i.end()) != a.i.end() or
		find_first_of(a.i.begin(), a.i.end(), b.j.begin(), b.j.end()) != a.i.end() or
		find_first_of(a.j.begin(), a.j.end(), b.i.begin(), b.i.end()) != a.j.end() or
		find_first_of(a.j.begin(), a.j.end(), b.j.begin(), b.j.end()) != a.j.end();
}

// --------------------------------------------------------------------

void CalculateBetaSheets(std::vector<Res>& inResidues, DSSP_Statistics& stats)
{
	// Calculate Bridges
	std::vector<Bridge> bridges;
	if (inResidues.size() > 4)
	{
		for (uint32_t i = 1; i + 4 < inResidues.size(); ++i)
		{
			auto& ri = inResidues[i];
			
			for (uint32_t j = i + 3; j + 1 < inResidues.size(); ++j)
			{
				auto& rj = inResidues[j];
				
				BridgeType type = TestBridge(ri, rj);
				if (type == btNoBridge)
					continue;
				
				bool found = false;
				for (Bridge& bridge : bridges)
				{
					if (type != bridge.type or i != bridge.i.back() + 1)
						continue;
					
					if (type == btParallel and bridge.j.back() + 1 == j)
					{
						bridge.i.push_back(i);
						bridge.j.push_back(j);
						found = true;
						break;
					}
	
					if (type == btAntiParallel and bridge.j.front() - 1 == j)
					{
						bridge.i.push_back(i);
						bridge.j.push_front(j);
						found = true;
						break;
					}
				}
				
				if (not found)
				{
					Bridge bridge = {};
					
					bridge.type = type;
					bridge.i.push_back(i);
					bridge.chainI = ri.mM.asymID();
					bridge.j.push_back(j);
					bridge.chainJ = rj.mM.asymID();
					
					bridges.push_back(bridge);
				}
			}
		}
	}

	// extend ladders
	std::sort(bridges.begin(), bridges.end());
	
	for (uint32_t i = 0; i < bridges.size(); ++i)
	{
		for (uint32_t j = i + 1; j < bridges.size(); ++j)
		{
			uint32_t ibi = bridges[i].i.front();
			uint32_t iei = bridges[i].i.back();
			uint32_t jbi = bridges[i].j.front();
			uint32_t jei = bridges[i].j.back();
			uint32_t ibj = bridges[j].i.front();
			uint32_t iej = bridges[j].i.back();
			uint32_t jbj = bridges[j].j.front();
			uint32_t jej = bridges[j].j.back();

			if (bridges[i].type != bridges[j].type or
				NoChainBreak(inResidues[std::min(ibi, ibj)], inResidues[std::max(iei, iej)]) == false or
				NoChainBreak(inResidues[std::min(jbi, jbj)], inResidues[std::max(jei, jej)]) == false or
				ibj - iei >= 6 or
				(iei >= ibj and ibi <= iej))
			{
				continue;
			}
			
			bool bulge;
			if (bridges[i].type == btParallel)
				bulge = ((jbj - jei < 6 and ibj - iei < 3) or (jbj - jei < 3));
			else
				bulge = ((jbi - jej < 6 and ibj - iei < 3) or (jbi - jej < 3));

			if (bulge)
			{
				bridges[i].i.insert(bridges[i].i.end(), bridges[j].i.begin(), bridges[j].i.end());
				if (bridges[i].type == btParallel)
					bridges[i].j.insert(bridges[i].j.end(), bridges[j].j.begin(), bridges[j].j.end());
				else
					bridges[i].j.insert(bridges[i].j.begin(), bridges[j].j.begin(), bridges[j].j.end());
				bridges.erase(bridges.begin() + j);
				--j;
			}
		}
	}

	// Sheet
	std::set<Bridge*> ladderset;
	for (Bridge& bridge : bridges)
	{
		ladderset.insert(&bridge);
		
		uint32_t n = bridge.i.size();
		if (n > kHistogramSize)
			n = kHistogramSize;
		
		if (bridge.type == btParallel)
			stats.parallelBridgesPerLadderHistogram[n - 1] += 1;
		else
			stats.antiparallelBridgesPerLadderHistogram[n - 1] += 1;
	}
	
	uint32_t sheet = 1, ladder = 0;
	while (not ladderset.empty())
	{
		std::set<Bridge*> sheetset;
		sheetset.insert(*ladderset.begin());
		ladderset.erase(ladderset.begin());

		bool done = false;
		while (not done)
		{
			done = true;
			for (Bridge* a : sheetset)
			{
				for (Bridge* b : ladderset)
				{
					if (Linked(*a, *b))
					{
						sheetset.insert(b);
						ladderset.erase(b);
						done = false;
						break;
					}
				}
				if (not done)
					break;
			}
		}

		for (Bridge* bridge : sheetset)
		{
			bridge->ladder = ladder;
			bridge->sheet = sheet;
			bridge->link = sheetset;
			
			++ladder;
		}
		
		uint32_t nrOfLaddersPerSheet = sheetset.size();
		if (nrOfLaddersPerSheet > kHistogramSize)
			nrOfLaddersPerSheet = kHistogramSize;
		if (nrOfLaddersPerSheet == 1 and (*sheetset.begin())->i.size() > 1)
			stats.laddersPerSheetHistogram[0] += 1;
		else if (nrOfLaddersPerSheet > 1)
			stats.laddersPerSheetHistogram[nrOfLaddersPerSheet - 1] += 1;
		
		++sheet;
	}

	for (Bridge& bridge : bridges)
	{
		// find out if any of the i and j set members already have
		// a bridge assigned, if so, we're assigning bridge 2
		
		uint32_t betai = 0, betaj = 0;
		
		for (uint32_t l : bridge.i)
		{
			if (inResidues[l].GetBetaPartner(0).residue != nullptr)
			{
				betai = 1;
				break;
			}
		}

		for (uint32_t l : bridge.j)
		{
			if (inResidues[l].GetBetaPartner(0).residue != nullptr)
			{
				betaj = 1;
				break;
			}
		}
		
		SecondaryStructureType ss = ssBetabridge;
		if (bridge.i.size() > 1)
			ss = ssStrand;
		
		if (bridge.type == btParallel)
		{
			stats.nrOfHBondsInParallelBridges += bridge.i.back() - bridge.i.front() + 2;
			
			std::deque<uint32_t>::iterator j = bridge.j.begin();
			for (uint32_t i : bridge.i)
				inResidues[i].SetBetaPartner(betai, inResidues[*j++], bridge.ladder, true);

			j = bridge.i.begin();
			for (uint32_t i : bridge.j)
				inResidues[i].SetBetaPartner(betaj, inResidues[*j++], bridge.ladder, true);
		}
		else
		{
			stats.nrOfHBondsInAntiparallelBridges += bridge.i.back() - bridge.i.front() + 2;

			std::deque<uint32_t>::reverse_iterator j = bridge.j.rbegin();
			for (uint32_t i : bridge.i)
				inResidues[i].SetBetaPartner(betai, inResidues[*j++], bridge.ladder, false);

			j = bridge.i.rbegin();
			for (uint32_t i : bridge.j)
				inResidues[i].SetBetaPartner(betaj, inResidues[*j++], bridge.ladder, false);
		}

		for (uint32_t i = bridge.i.front(); i <= bridge.i.back(); ++i)
		{
			if (inResidues[i].GetSecondaryStructure() != ssStrand)
				inResidues[i].SetSecondaryStructure(ss);
			inResidues[i].SetSheet(bridge.sheet);
		}

		for (uint32_t i = bridge.j.front(); i <= bridge.j.back(); ++i)
		{
			if (inResidues[i].GetSecondaryStructure() != ssStrand)
				inResidues[i].SetSecondaryStructure(ss);
			inResidues[i].SetSheet(bridge.sheet);
		}
	}
}

// --------------------------------------------------------------------
// TODO: improve alpha helix calculation by better recognizing pi-helices 

void CalculateAlphaHelices(std::vector<Res>& inResidues, DSSP_Statistics& stats, bool inPreferPiHelices = true)
{
	// Helix and Turn
	for (uint32_t stride = 3; stride <= 5; ++stride)
	{
		for (uint32_t i = 0; i + stride < inResidues.size(); ++i)
		{
			if (NoChainBreak(inResidues[i], inResidues[i + stride]) and TestBond(&inResidues[i + stride], &inResidues[i]))
			{
				inResidues[i + stride].SetHelixFlag(stride, Helix::End);
				for (uint32_t j = i + 1; j < i + stride; ++j)
				{
					if (inResidues[j].GetHelixFlag(stride) == Helix::None)
						inResidues[j].SetHelixFlag(stride, Helix::Middle);
				}
				
				if (inResidues[i].GetHelixFlag(stride) == Helix::End)
					inResidues[i].SetHelixFlag(stride, Helix::StartAndEnd);
				else
					inResidues[i].SetHelixFlag(stride, Helix::Start);
			}
		}
	}
	
	for (auto& r : inResidues)
	{
		double kappa = r.mM.kappa();
		r.SetBend(kappa != 360 and kappa > 70);
	}

	for (uint32_t i = 1; i + 4 < inResidues.size(); ++i)
	{
		if (inResidues[i].IsHelixStart(4) and inResidues[i - 1].IsHelixStart(4))
		{
			for (uint32_t j = i; j <= i + 3; ++j)
				inResidues[j].SetSecondaryStructure(ssAlphahelix);
		}
	}

	for (uint32_t i = 1; i + 3 < inResidues.size(); ++i)
	{
		if (inResidues[i].IsHelixStart(3) and inResidues[i - 1].IsHelixStart(3))
		{
			bool empty = true;
			for (uint32_t j = i; empty and j <= i + 2; ++j)
				empty = inResidues[j].GetSecondaryStructure() == ssLoop or inResidues[j].GetSecondaryStructure() == ssHelix_3;
			if (empty)
			{
				for (uint32_t j = i; j <= i + 2; ++j)
					inResidues[j].SetSecondaryStructure(ssHelix_3);
			}
		}
	}

	for (uint32_t i = 1; i + 5 < inResidues.size(); ++i)
	{
		if (inResidues[i].IsHelixStart(5) and inResidues[i - 1].IsHelixStart(5))
		{
			bool empty = true;
			for (uint32_t j = i; empty and j <= i + 4; ++j)
				empty = inResidues[j].GetSecondaryStructure() == ssLoop or inResidues[j].GetSecondaryStructure() == ssHelix_5 or
							(inPreferPiHelices and inResidues[j].GetSecondaryStructure() == ssAlphahelix);
			if (empty)
			{
				for (uint32_t j = i; j <= i + 4; ++j)
					inResidues[j].SetSecondaryStructure(ssHelix_5);
			}
		}
	}
			
	for (uint32_t i = 1; i + 1 < inResidues.size(); ++i)
	{
		if (inResidues[i].GetSecondaryStructure() == ssLoop)
		{
			bool isTurn = false;
			for (uint32_t stride = 3; stride <= 5 and not isTurn; ++stride)
			{
				for (uint32_t k = 1; k < stride and not isTurn; ++k)
					isTurn = (i >= k) and inResidues[i - k].IsHelixStart(stride);
			}
			
			if (isTurn)
				inResidues[i].SetSecondaryStructure(ssTurn);
			else if (inResidues[i].IsBend())
				inResidues[i].SetSecondaryStructure(ssBend);
		}
	}

	std::string asym;
	size_t helixLength = 0;
	for (auto r: inResidues)
	{
		if (r.mM.asymID() != asym)
		{
			helixLength = 0;
			asym = r.mM.asymID();
		}

		if (r.GetSecondaryStructure() == ssAlphahelix)
			++helixLength;
		else if (helixLength > 0)
		{
			if (helixLength > kHistogramSize)
				helixLength = kHistogramSize;

			stats.residuesPerAlphaHelixHistogram[helixLength - 1] += 1;
			helixLength = 0;
		}
	}
}

// --------------------------------------------------------------------

struct DSSPImpl
{
	DSSPImpl(const Structure& s);
	
	const Structure&			mStructure;
	const std::list<Polymer>&	mPolymers;
	std::vector<Res>			mResidues;
	std::vector<std::pair<Res*,Res*>> mSSBonds;

	auto findRes(const std::string& asymID, int seqID)
	{
		return std::find_if(mResidues.begin(), mResidues.end(), [&](auto& r) { return r.mM.asymID() == asymID and r.mM.seqID() == seqID; });
	}

	DSSP_Statistics				mStats = {};
};

// --------------------------------------------------------------------

DSSPImpl::DSSPImpl(const Structure& s)
	: mStructure(s)
	, mPolymers(mStructure.polymers())
{
	if (cif::VERBOSE)
		std::cerr << "Calculating DSSP ";

	size_t nRes = accumulate(mPolymers.begin(), mPolymers.end(),
		0.0, [](double s, auto& p) { return s + p.size(); });

	mStats.nrOfResidues = nRes;
	mStats.nrOfChains = mPolymers.size();

	mResidues.reserve(nRes);
	int resNumber = 0;
	
	for (auto& p: mPolymers)
	{
		for (auto& m: p)
		{
			if (not m.isComplete())
				continue;
			
			++resNumber;

			if (not mResidues.empty() and
				Distance(mResidues.back().mC, m.atomByID("N").location()) > kMaxPeptideBondLength)
			{
				++mStats.nrOfChains;
				++resNumber;
			}

			mResidues.emplace_back(m, resNumber);
		}
	}
	
	for (size_t i = 0; i + 1 < mResidues.size(); ++i)
	{
		mResidues[i].mNext = &mResidues[i + 1];
		mResidues[i + 1].mPrev = &mResidues[i];
		
		mResidues[i + 1].assignHydrogen();
	}

	std::thread ta(std::bind(&CalculateAccessibilities, std::ref(mResidues), std::ref(mStats)));

	auto& db = s.getFile().data();
	for (auto r: db["struct_conn"].find(cif::Key("conn_type_id") == "disulf"))
	{
		std::string asym1, asym2;
		int seq1, seq2;
		cif::tie(asym1, seq1, asym2, seq2) = r.get("ptnr1_label_asym_id", "ptnr1_label_seq_id", "ptnr2_label_asym_id", "ptnr2_label_seq_id");

		auto r1 = findRes(asym1, seq1);
		if (r1 == mResidues.end())
			throw std::runtime_error("Invalid file, missing residue for SS bond");

		auto r2 = findRes(asym2, seq2);
		if (r2 == mResidues.end())
			throw std::runtime_error("Invalid file, missing residue for SS bond");

		mSSBonds.emplace_back(&*r1, &*r2);
	}

	if (cif::VERBOSE) std::cerr << ".";
	CalculateHBondEnergies(mResidues);

	if (cif::VERBOSE) std::cerr << ".";
	CalculateBetaSheets(mResidues, mStats);

	if (cif::VERBOSE) std::cerr << ".";
	CalculateAlphaHelices(mResidues, mStats);

	if (cif::VERBOSE) std::cerr << std::endl;

	if (cif::VERBOSE > 1)
	{
		for (auto& r: mResidues)
		{
			auto& m = r.mM;
			
			char helix[4] = { };
			for (size_t stride: { 3, 4, 5 })
			{
				switch (r.GetHelixFlag(stride))
				{
					case Helix::Start:			helix[stride - 3] = '>'; break;
					case Helix::Middle:			helix[stride - 3] = '0' + stride; break;
					case Helix::StartAndEnd:	helix[stride - 3] = 'X'; break;
					case Helix::End:			helix[stride - 3] = '<'; break;
					case Helix::None:			helix[stride - 3] = ' '; break;
				}
			}
			
			auto id = m.asymID() + ':' + std::to_string(m.seqID()) + '/' + m.compoundID();
			
			std::cerr << id << std::string(12 - id.length(), ' ')
					  << char(r.mSecondaryStructure) << ' '
					  << helix
					  << std::endl;
		}
	}

	// finish statistics
	mStats.nrOfSSBridges = mSSBonds.size();

	mStats.nrOfIntraChainSSBridges = 0;
	for (const auto& [a, b]: mSSBonds)
	{
		if (a->mM.asymID() != b->mM.asymID())
			++mStats.nrOfIntraChainSSBridges;
	}

	mStats.nrOfHBonds = 0;
	for (auto& r: mResidues)
	{
		auto donor = r.mHBondDonor;

		for (int i = 0; i < 2; ++i)
		{
			if (donor[i].residue != nullptr and donor[i].energy < kMaxHBondEnergy)
			{
				++mStats.nrOfHBonds;
				auto k = donor[i].residue->mNumber - r.mNumber;
				if (k >= -5 and k <= 5)
					mStats.nrOfHBondsPerDistance[k + 5] += 1;
			}
		}
	}

	ta.join();
}

// --------------------------------------------------------------------

DSSP::ResidueInfo::ResidueInfo(Res* res)
	: mImpl(res)
{
}

const Monomer& DSSP::ResidueInfo::residue() const
{
	return mImpl->mM;
}

ChainBreak DSSP::ResidueInfo::chainBreak() const
{
	return ChainBreak::None;
}

int DSSP::ResidueInfo::nr() const
{
	return mImpl->mNumber;
}

SecondaryStructureType DSSP::ResidueInfo::ss() const
{
	return mImpl->mSecondaryStructure;
}

int DSSP::ResidueInfo::ssBridgeNr() const
{
	return 0;
}

Helix DSSP::ResidueInfo::helix(int stride) const
{
	return mImpl->GetHelixFlag(stride);
}

bool DSSP::ResidueInfo::bend() const
{
	return mImpl->IsBend();
}

double DSSP::ResidueInfo::accessibility() const
{
	return mImpl->mAccessibility;
}

std::tuple<DSSP::ResidueInfo,int,bool> DSSP::ResidueInfo::bridgePartner(int i) const
{
	auto bp = mImpl->GetBetaPartner(i);

	ResidueInfo ri(bp.residue);

	return std::make_tuple(std::move(ri), bp.ladder, bp.parallel);
}

int DSSP::ResidueInfo::sheet() const
{
	return mImpl->GetSheet();
}

std::tuple<DSSP::ResidueInfo,double> DSSP::ResidueInfo::acceptor(int i) const
{
	auto& a = mImpl->mHBondAcceptor[i];
	return { ResidueInfo(a.residue), a.energy };
}

std::tuple<DSSP::ResidueInfo,double> DSSP::ResidueInfo::donor(int i) const
{
	auto& d = mImpl->mHBondDonor[i];
	return { ResidueInfo(d.residue), d.energy };
}

// --------------------------------------------------------------------

DSSP::iterator::iterator(res_iter cur)
	: mCurrent(&*cur)
{
}

DSSP::iterator::iterator(const iterator& i)
	: mCurrent(i.mCurrent)
{
}

DSSP::iterator& DSSP::iterator::operator=(const iterator& i)
{
	mCurrent = i.mCurrent;
	return *this;
}

DSSP::iterator& DSSP::iterator::operator++()
{
	++mCurrent.mImpl;
	return *this;
}

// --------------------------------------------------------------------

DSSP::DSSP(const Structure& s)
	: mImpl(new DSSPImpl(s))
{
}

DSSP::~DSSP()
{
	delete mImpl;
}

DSSP::iterator DSSP::begin() const
{
	return iterator(mImpl->mResidues.begin());
}

DSSP::iterator DSSP::end() const
{
	return iterator(mImpl->mResidues.end());
}

SecondaryStructureType DSSP::operator()(const std::string& inAsymID, int inSeqID) const
{
	SecondaryStructureType result = ssLoop;
	auto i = find_if(mImpl->mResidues.begin(), mImpl->mResidues.end(),
		[&](auto& r) { return r.mM.asymID() == inAsymID and r.mM.seqID() == inSeqID; });
	if (i != mImpl->mResidues.end())
		result = i->mSecondaryStructure;
	else if (cif::VERBOSE)
		std::cerr << "Could not find secondary structure for " << inAsymID << ':' << inSeqID << std::endl;
	return result;
}

SecondaryStructureType DSSP::operator()(const Monomer& m) const
{
	return operator()(m.asymID(), m.seqID());
}

double DSSP::accessibility(const std::string& inAsymID, int inSeqID) const
{
	SecondaryStructureType result = ssLoop;
	auto i = find_if(mImpl->mResidues.begin(), mImpl->mResidues.end(),
		[&](auto& r) { return r.mM.asymID() == inAsymID and r.mM.seqID() == inSeqID; });
	if (i != mImpl->mResidues.end())
		result = i->mSecondaryStructure;
	else if (cif::VERBOSE)
		std::cerr << "Could not find secondary structure for " << inAsymID << ':' << inSeqID << std::endl;
	return result;
}

double DSSP::accessibility(const Monomer& m) const
{
	return accessibility(m.asymID(), m.seqID());
}

bool DSSP::isAlphaHelixEndBeforeStart(const Monomer& m) const
{
	return isAlphaHelixEndBeforeStart(m.asymID(), m.seqID());
}

bool DSSP::isAlphaHelixEndBeforeStart(const std::string& inAsymID, int inSeqID) const
{
	auto i = find_if(mImpl->mResidues.begin(), mImpl->mResidues.end(),
		[&](auto& r) { return r.mM.asymID() == inAsymID and r.mM.seqID() == inSeqID; });

	bool result = false;

	if (i != mImpl->mResidues.end() and i + 1 != mImpl->mResidues.end())
		result = i->GetHelixFlag(4) == Helix::End and (i + 1)->GetHelixFlag(4) == Helix::Start;
	else if (cif::VERBOSE)
		std::cerr << "Could not find secondary structure for " << inAsymID << ':' << inSeqID << std::endl;

	return result;
}

DSSP_Statistics DSSP::GetStatistics() const
{
	return mImpl->mStats;
}

}
