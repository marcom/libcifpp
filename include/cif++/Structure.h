// Lib for working with structures as contained in mmCIF and PDB files

#pragma once

#include <boost/filesystem/operations.hpp>
#include <boost/math/quaternion.hpp>

#include <boost/any.hpp>

#include <clipper/core/coords.h>

#include "cif++/AtomType.h"
#include "cif++/Point.h"
#include "cif++/Compound.h"
#include "cif++/Cif++.h"

/*
	To modify a structure, you will have to use actions.
	
	The currently supported actions are:
	
//	- Move atom to new location
	- Remove atom
//	- Add new atom that was formerly missing
//	- Add alternate Residue
	- 
	
	Other important design principles:
	
	- all objects here are references to the actual data. Not models of
	  the data itself. That means that if you copy an atom, you copy the
	  reference to an atom in the structure. You're not creating a new
	  atom. This may sound obvious, but it is not if you are used to
	  copy semantics in the C++ world.
	
	
*/

//// forward declaration
//namespace cif
//{
//	class Category;
//	class Datablock;
//	class File;
//	class RowSet;
//};

namespace mmcif
{

class Atom;
class Residue;
class Monomer;
class Polymer;
class Structure;
class File;

// --------------------------------------------------------------------
// We do not want to introduce a dependency on cif++ here, we might want
// to change the backend storage in the future.
// So, in order to access the data we use properties based on boost::any
// Eventually this should be moved to std::variant, but that's only when
// c++17 is acceptable.

struct Property
{
	Property() {}
	Property(const std::string& name, const boost::any& value)
		: name(name), value(value) {}
	
	std::string name;
	boost::any value;
};

typedef std::vector<Property>	PropertyList;

// --------------------------------------------------------------------

class Atom
{
  public:
//	Atom(const structure& s, const std::string& id);
	Atom(struct AtomImpl* impl);
	Atom(const File& f, const std::string& id);
	Atom(const Atom& rhs);

	~Atom();
	
	Atom& operator=(const Atom& rhs);

	const std::string& id() const;
	AtomType type() const;

	Point location() const;
	void location(Point p);

	const Compound& comp() const;
	const Entity& ent() const;
	bool isWater() const;
	int charge() const;

	float uIso() const;
	float occupancy() const;
	
	template<typename T>
	T property(const std::string& name) const;
	
	template<typename T>
	void property(const std::string& name, const T& value);
	
	// specifications
	std::string labelAtomId() const;
	std::string labelCompId() const;
	std::string labelAsymId() const;
	int labelSeqId() const;
	std::string labelAltId() const;
	
	std::string authAtomId() const;
	std::string authCompId() const;
	std::string authAsymId() const;
	int authSeqId() const;
	std::string pdbxAuthInsCode() const;
	std::string authAltId() const;
	
	bool operator==(const Atom& rhs) const;

	const File& getFile() const;
	
	// get clipper format Atom
	clipper::Atom toClipper() const;

	// Radius calculation based on integrating the density until perc of electrons is found
	void calculateRadius(float resHigh, float resLow, float perc);
	float radius() const;


	// the energy-type field
	std::string energyType() const;

  private:
 	struct AtomImpl*			mImpl;
};

typedef std::vector<Atom> AtomView;

// --------------------------------------------------------------------

class Residue
{
  public:
	Residue();
	Residue(const Residue& rhs);
	Residue& operator=(const Residue& rhs);

	Residue(const Structure& structure)
		: mStructure(&structure) {}
	Residue(const Structure& structure, const std::string& compoundID,
		const std::string& asymID, int seqID = 0,
		const std::string& altID = "")
		: mStructure(&structure), mCompoundID(compoundID)
		, mAsymID(asymID), mAltID(altID), mSeqID(seqID) {}
	
	const Compound&		compound() const;
	AtomView			atoms() const;
	
	Atom				atomByID(const std::string& atomID) const;

	const std::string&	compoundID() const	{ return mCompoundID; }
	const std::string&	asymID() const		{ return mAsymID; }
	int					seqID() const		{ return mSeqID; }
	const std::string&	altID() const		{ return mAltID; }
	
	// Is this residue a single entity?
	bool isEntity() const;
	
	const Structure& structure() const		{ return *mStructure; }

  protected:

	const Structure* mStructure;
	std::string	mCompoundID, mAsymID, mAltID;
	int mSeqID;
	mutable AtomView mAtoms;
};

// --------------------------------------------------------------------
// a monomer models a single Residue in a protein chain 

class Monomer : public Residue
{
  public:
	Monomer();
	Monomer(const Monomer& rhs);
	Monomer& operator=(const Monomer& rhs);

	Monomer(Polymer& polymer, uint32 index);
	Monomer(Polymer& polymer, uint32 index, int seqID,
		const std::string& compoundID, const std::string& altID);

	// Assuming this is really an amino acid...
	
	float phi() const;
	float psi() const;
	float alpha() const;
	float kappa() const;

	Atom CAlpha() const		{ return atomByID("CA"); }
	Atom C() const			{ return atomByID("C"); }
	Atom N() const			{ return atomByID("N"); }
	Atom O() const			{ return atomByID("O"); }
	Atom H() const			{ return atomByID("H"); }

  private:
	Polymer*	mPolymer;
	uint32		mIndex;
};

// --------------------------------------------------------------------

class Polymer
{
  public:
	Polymer(const Structure& s, const std::string& entityID, const std::string& asymID);
	Polymer(const Polymer&);
	Polymer& operator=(const Polymer&);
	
	struct iterator : public std::iterator<std::random_access_iterator_tag, Monomer>
	{
		typedef std::iterator<std::bidirectional_iterator_tag, Monomer>	base_type;
		typedef base_type::reference									reference;
		typedef base_type::pointer										pointer;
		
		iterator(Polymer& p, uint32 index);
		iterator(iterator&& rhs);

		iterator(const iterator& rhs);
		iterator& operator=(const iterator& rhs);
//		iterator& operator=(iterator&& rhs);
		
		reference	operator*()											{ return mCurrent; }
		pointer		operator->()										{ return &mCurrent; }
		
		iterator&	operator++();
		iterator	operator++(int)
		{
			iterator result(*this);
			operator++();
			return result;
		}

		iterator&	operator--();
		iterator	operator--(int)
		{
			iterator result(*this);
			operator--();
			return result;
		}

		bool		operator==(const iterator& rhs) const				{ return mPolymer == rhs.mPolymer and mIndex == rhs.mIndex; }
		bool		operator!=(const iterator& rhs) const				{ return mPolymer != rhs.mPolymer or mIndex != rhs.mIndex; }

	  private:
		Polymer*	mPolymer;
		uint32		mIndex;
		Monomer		mCurrent;
	};

	iterator begin();
	iterator end();
	
	size_t size() const							{ return mPolySeq.size(); }
	Monomer operator[](size_t index) const		{ return Monomer(const_cast<Polymer&>(*this), index); }

	Structure* structure() const	{ return mStructure; }
	
	std::string asymID() const		{ return mAsymID; }
	std::string entityID() const	{ return mEntityID; }

  private:

	friend struct iterator;

	Structure*					mStructure;
	std::string					mEntityID;
	std::string					mAsymID;
	cif::RowSet					mPolySeq;
};

// --------------------------------------------------------------------
// file is a reference to the data stored in e.g. the cif file.
// This object is not copyable.

class File : public std::enable_shared_from_this<File>
{
  public:
	File();
	File(boost::filesystem::path p);
	~File();

	File(const File&) = delete;
	File& operator=(const File&) = delete;

	void load(boost::filesystem::path p);
	void save(boost::filesystem::path p);
	
	Structure* model(size_t nr = 1);

	struct FileImpl& impl() const						{ return *mImpl; }

	std::vector<const Entity*> entities();

	cif::Datablock& data();
	cif::File& file();

  private:

	struct FileImpl*	mImpl;
};

// --------------------------------------------------------------------

class Structure
{
  public:
	Structure(File& p, uint32 modelNr = 1);
	Structure(const Structure&);
	Structure& operator=(const Structure&);
	~Structure();

	File& getFile() const;

	AtomView atoms() const;
	AtomView waters() const;
	
	std::vector<Polymer> polymers() const;
	std::vector<Residue> nonPolymers() const;

	Atom getAtomById(std::string id) const;
	Atom getAtomByLocation(Point pt, float maxDistance) const;
	
	Atom getAtomForLabel(const std::string& atomId, const std::string& asymId,
		const std::string& compId, int seqId, const std::string& altId = "");
	
	Atom getAtomForAuth(const std::string& atomId, const std::string& asymId,
		const std::string& compId, int seqId, const std::string& altId = "",
		const std::string& pdbxAuthInsCode = "");
	
	// map between auth and label locations
	
	std::tuple<std::string,int,std::string> MapAuthToLabel(const std::string& asymId,
		const std::string& seqId, const std::string& compId, const std::string& insCode = "");
	
	std::tuple<std::string,std::string,std::string,std::string> MapLabelToAuth(
		const std::string& asymId, int seqId, const std::string& compId);

	// returns chain, seqnr
	std::tuple<std::string,std::string> MapLabelToAuth(
		const std::string& asymId, int seqId);

	// returns chain,seqnr,comp,iCode
	std::tuple<std::string,int,std::string,std::string> MapLabelToPDB(
		const std::string& asymId, int seqId, const std::string& compId);

	std::tuple<std::string,int,std::string,std::string> MapPDBToLabel(
		const std::string& asymId, int seqId, const std::string& compId, const std::string& iCode);
	
	// Actions
	void removeAtom(Atom& a);
	void swapAtoms(Atom& a1, Atom& a2);	// swap the labels for these atoms
	void moveAtom(Atom& a, Point p);	// move atom to a new location
	void changeResidue(Residue& res, const std::string& newCompound,
		const std::vector<std::tuple<std::string,std::string>>& remappedAtoms);
	
  private:
	friend Polymer;
	friend Residue;

	cif::Category& category(const char* name) const;
	cif::Datablock& datablock() const;

	struct StructureImpl*	mImpl;
};

}
