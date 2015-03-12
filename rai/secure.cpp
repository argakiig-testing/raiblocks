#include <rai/secure.hpp>

#include <rai/working.hpp>

#include <boost/property_tree/json_parser.hpp>

#include <blake2/blake2.h>

#include <ed25519-donna/ed25519.h>

// Genesis keys for network variants
namespace
{
    std::string rai_test_private_key = "34F0A37AAD20F4A260F0A5B3CB3D7FB50673212263E58A380BC10474BB039CE4";
    std::string rai_test_public_key = "FA5B51D063BADDF345EFD7EF0D3C5FB115C85B1EF4CDE89D8B7DF3EAF60A04A4"; // U8NzqpKcVn8AFrFd8YVSS6LvYHfufveHwvYhwCoGbDqth5FjVu
    std::string rai_beta_public_key = "25FEB3B0C2506F953A9CA97281954EA161A03DA04ED0E558AA978A6F08AF56FC"; // SHA57oWXA6S6BogbvVvhUZY2FQ1WAuLGYvcPTxvLncFdHtghvK
    std::string rai_live_public_key = "0";
}

size_t constexpr rai::send_block::size;
size_t constexpr rai::receive_block::size;
size_t constexpr rai::open_block::size;
size_t constexpr rai::change_block::size;

rai::keypair const rai::test_genesis_key (rai_test_private_key);
rai::account const rai::rai_test_account (rai_test_public_key);
rai::account const rai::rai_beta_account (rai_beta_public_key);
rai::account const rai::rai_live_account (rai_live_public_key);

rai::account const rai::genesis_account = rai_network == rai_networks::rai_test_network ? rai_test_account : rai_network == rai_networks::rai_beta_network ? rai_beta_account : rai_live_account;
rai::uint128_t const rai::genesis_amount = std::numeric_limits <rai::uint128_t>::max ();

boost::filesystem::path rai::working_path ()
{
	auto result (rai::app_path ());
	switch (rai::rai_network)
	{
		case rai::rai_networks::rai_test_network:
			result /= "RaiBlocksTest";
			break;
		case rai::rai_networks::rai_beta_network:
			result /= "RaiBlocksBeta";
			break;
		case rai::rai_networks::rai_live_network:
			result /= "RaiBlocks";
			break;
	}
	return result;
}

size_t rai::unique_ptr_block_hash::operator () (std::unique_ptr <rai::block> const & block_a) const
{
	auto hash (block_a->hash ());
	auto result (static_cast <size_t> (hash.qwords [0]));
	return result;
}

bool rai::unique_ptr_block_hash::operator () (std::unique_ptr <rai::block> const & lhs, std::unique_ptr <rai::block> const & rhs) const
{
	return *lhs == *rhs;
}

bool rai::votes::vote (rai::vote const & vote_a)
{
	auto result (false);
	// Reject unsigned votes
	if (!rai::validate_message (vote_a.account, vote_a.hash (), vote_a.signature))
	{
		// Check if we're adding a new vote entry or modifying an existing one.
		auto existing (rep_votes.find (vote_a.account));
		if (existing == rep_votes.end ())
		{
			result = true;
			rep_votes.insert (std::make_pair (vote_a.account, std::make_pair (vote_a.sequence, vote_a.block->clone ())));
		}
		else
		{
			// Only accept votes with an increasing sequence number
			if (existing->second.first < vote_a.sequence)
			{
				result = !(*existing->second.second == *vote_a.block);
				if (result)
				{
					existing->second.second = vote_a.block->clone ();
				}
			}
		}
	}
	return result;
}

// Sum the weights for each vote and return the winning block with its vote tally
std::pair <rai::uint128_t, std::unique_ptr <rai::block>> rai::ledger::winner (MDB_txn * transaction_a, rai::votes const & votes_a)
{
	auto tally_l (tally (transaction_a, votes_a));
	auto existing (tally_l.begin ());
	return std::make_pair (existing->first, existing->second->clone ());
}

std::map <rai::uint128_t, std::unique_ptr <rai::block>, std::greater <rai::uint128_t>> rai::ledger::tally (MDB_txn * transaction_a, rai::votes const & votes_a)
{
	std::unordered_map <std::unique_ptr <block>, rai::uint128_t, rai::unique_ptr_block_hash, rai::unique_ptr_block_hash> totals;
	// Construct a map of blocks -> vote total.
	for (auto & i: votes_a.rep_votes)
	{
		auto existing (totals.find (i.second.second));
		if (existing == totals.end ())
		{
			totals.insert (std::make_pair (i.second.second->clone (), 0));
			existing = totals.find (i.second.second);
			assert (existing != totals.end ());
		}
		auto weight_l (weight (transaction_a, i.first));
		existing->second += weight_l;
	}
	// Construction a map of vote total -> block in decreasing order.
	std::map <rai::uint128_t, std::unique_ptr <rai::block>, std::greater <rai::uint128_t>> result;
	for (auto & i: totals)
	{
		result [i.second] = i.first->clone ();
	}
	return result;
}

rai::votes::votes (rai::block_hash const & id_a) :
// Sequence 0 is the first response by a representative before a fork was observed
sequence (1),
id (id_a)
{
}

// Create a new random keypair
rai::keypair::keypair ()
{
    random_pool.GenerateBlock (prv.bytes.data (), prv.bytes.size ());
	ed25519_publickey (prv.bytes.data (), pub.bytes.data ());
}

// Create a keypair given a hex string of the private key
rai::keypair::keypair (std::string const & prv_a)
{
	auto error (prv.decode_hex (prv_a));
	assert (!error);
	ed25519_publickey (prv.bytes.data (), pub.bytes.data ());
}

namespace
{
class xorshift1024star
{
public:
    xorshift1024star ():
    p (0)
    {
    }
    std::array <uint64_t, 16> s;
    unsigned p;
    uint64_t next ()
    {
        auto p_l (p);
        auto pn ((p_l + 1) & 15);
        p = pn;
        uint64_t s0 = s[ p_l ];
        uint64_t s1 = s[ pn ];
        s1 ^= s1 << 31; // a
        s1 ^= s1 >> 11; // b
        s0 ^= s0 >> 30; // c
        return ( s[ pn ] = s0 ^ s1 ) * 1181783497276652981LL;
    }
};
}

namespace {
    size_t constexpr stepping (16);
}
rai::kdf::kdf (size_t entries_a) :
entries (entries_a),
data (new uint64_t [entries_a])
{
    assert ((entries_a & (stepping - 1)) == 0);
}

// Derive a wallet key from a password and salt.
rai::uint256_union rai::kdf::generate (std::string const & password_a, rai::uint256_union const & salt_a)
{
    rai::uint256_union input;
    blake2b_state hash;
	blake2b_init (&hash, 32);
    blake2b_update (&hash, reinterpret_cast <uint8_t const *> (password_a.data ()), password_a.size ());
    blake2b_final (&hash, input.bytes.data (), input.bytes.size ());
    input ^= salt_a;
    blake2b_init (&hash, 32);
    auto entries_l (entries);
    auto mask (entries_l - 1);
    xorshift1024star rng;
    rng.s [0] = input.qwords [0];
    rng.s [1] = input.qwords [1];
    rng.s [2] = input.qwords [2];
    rng.s [3] = input.qwords [3];
    for (auto i (4), n (16); i != n; ++i)
    {
        rng.s [i] = 0;
    }
    // Random-fill buffer for an initialized starting point
    for (auto i (data.get ()), n (data.get () + entries_l); i != n; ++i)
    {
        auto next (rng.next ());
        *i = next;
    }
    auto previous (rng.next ());
    // Random-write buffer to break n+1 = f(n) relation
    for (size_t i (0), n (entries); i != n; ++i)
    {
        auto index (previous & mask);
        auto value (rng.next ());
		// Use the index from the previous random value so LSB (data[index]) != value
        data [index] = value;
    }
    // Random-read buffer to prevent partial memorization
    union
    {
        std::array <uint64_t, stepping> qwords;
        std::array <uint8_t, stepping * sizeof (uint64_t)> bytes;
    } value;
	// Hash the memory buffer to derive encryption key
    for (size_t i (0), n (entries); i != n; i += stepping)
    {
        for (size_t j (0), m (stepping); j != m; ++j)
        {
            auto index (rng.next () % (entries_l - (i + j)));
            value.qwords [j] = data [index];
            data [index] = data [entries_l - (i + j) - 1];
        }
        blake2b_update (&hash, reinterpret_cast <uint8_t *> (value.bytes.data ()), stepping * sizeof (uint64_t));
    }
    rai::uint256_union result;
    blake2b_final (&hash, result.bytes.data (), result.bytes.size ());
    return result;
}

rai::ledger::ledger (rai::block_store & store_a) :
store (store_a),
send_observer ([] (rai::send_block const &, rai::account const &) {}),
receive_observer ([] (rai::receive_block const &, rai::account const &) {}),
open_observer ([] (rai::open_block const &, rai::account const &) {}),
change_observer ([] (rai::change_block const &, rai::account const &) {})
{
}

void rai::send_block::visit (rai::block_visitor & visitor_a) const
{
	visitor_a.send_block (*this);
}

void rai::send_block::hash (blake2b_state & hash_a) const
{
	hashables.hash (hash_a);
}

uint64_t rai::send_block::block_work () const
{
    return work;
}

void rai::send_block::block_work_set (uint64_t work_a)
{
	assert (!rai::work_validate (root (), work_a));
    work = work_a;
}

rai::send_hashables::send_hashables (rai::account const & destination_a, rai::block_hash const & previous_a, rai::amount const & balance_a) :
destination (destination_a),
previous (previous_a),
balance (balance_a)
{
}

rai::send_hashables::send_hashables (bool & error_a, rai::stream & stream_a)
{
	error_a = rai::read (stream_a, destination.bytes);
	if (!error_a)
	{
		error_a = rai::read (stream_a, previous.bytes);
		if (!error_a)
		{
			error_a = rai::read (stream_a, balance.bytes);
		}
	}
}

rai::send_hashables::send_hashables (bool & error_a, boost::property_tree::ptree const & tree_a)
{
	try
	{
		auto destination_l (tree_a.get <std::string> ("destination"));
		auto previous_l (tree_a.get <std::string> ("previous"));
		auto balance_l (tree_a.get <std::string> ("balance"));
		error_a = destination.decode_base58check (destination_l);
		if (!error_a)
		{
			error_a = previous.decode_hex (previous_l);
			if (!error_a)
			{
				error_a = balance.decode_hex (balance_l);
			}
		}
	}
	catch (std::runtime_error const &)
	{
		error_a = true;
	}
}

void rai::send_hashables::hash (blake2b_state & hash_a) const
{
	auto status (blake2b_update (&hash_a, destination.bytes.data (), sizeof (destination.bytes)));
	assert (status == 0);
	status = blake2b_update (&hash_a, previous.bytes.data (), sizeof (previous.bytes));
	assert (status == 0);
	status = blake2b_update (&hash_a, balance.bytes.data (), sizeof (balance.bytes));
	assert (status == 0);
}

void rai::send_block::serialize (rai::stream & stream_a) const
{
	write (stream_a, hashables.destination.bytes);
	write (stream_a, hashables.previous.bytes);
	write (stream_a, hashables.balance.bytes);
	write (stream_a, signature.bytes);
    write (stream_a, work);
}

void rai::send_block::serialize_json (std::string & string_a) const
{
    boost::property_tree::ptree tree;
    tree.put ("type", "send");
    tree.put ("destination", hashables.destination.to_base58check ());
    std::string previous;
    hashables.previous.encode_hex (previous);
    tree.put ("previous", previous);
    std::string balance;
    hashables.balance.encode_hex (balance);
    tree.put ("balance", balance);
    std::string signature_l;
    signature.encode_hex (signature_l);
    tree.put ("work", rai::to_string_hex (work));
    tree.put ("signature", signature_l);
    std::stringstream ostream;
    boost::property_tree::write_json (ostream, tree);
    string_a = ostream.str ();
}

bool rai::send_block::deserialize (rai::stream & stream_a)
{
	auto result (false);
    result = read (stream_a, hashables.destination.bytes);
	if (!result)
	{
		result = read (stream_a, hashables.previous.bytes);
		if (!result)
		{
			result = read (stream_a, hashables.balance.bytes);
			if (!result)
			{
                result = read (stream_a, signature.bytes);
                if (!result)
                {
                    result = read (stream_a, work);
                }
			}
		}
	}
	return result;
}

bool rai::send_block::deserialize_json (boost::property_tree::ptree const & tree_a)
{
    auto result (false);
    try
    {
        assert (tree_a.get <std::string> ("type") == "send");
        auto destination_l (tree_a.get <std::string> ("destination"));
        auto previous_l (tree_a.get <std::string> ("previous"));
        auto balance_l (tree_a.get <std::string> ("balance"));
        auto work_l (tree_a.get <std::string> ("work"));
        auto signature_l (tree_a.get <std::string> ("signature"));
        result = hashables.destination.decode_base58check (destination_l);
        if (!result)
        {
            result = hashables.previous.decode_hex (previous_l);
            if (!result)
            {
                result = hashables.balance.decode_hex (balance_l);
                if (!result)
                {
                    result = rai::from_string_hex (work_l, work);
                    if (!result)
                    {
                        result = signature.decode_hex (signature_l);
                    }
                }
            }
        }
    }
    catch (std::runtime_error const &)
    {
        result = true;
    }
    return result;
}

void rai::receive_block::visit (rai::block_visitor & visitor_a) const
{
    visitor_a.receive_block (*this);
}

bool rai::receive_block::operator == (rai::receive_block const & other_a) const
{
	auto result (hashables.previous == other_a.hashables.previous && hashables.source == other_a.hashables.source && work == other_a.work && signature == other_a.signature);
	return result;
}

bool rai::receive_block::deserialize (rai::stream & stream_a)
{
	auto result (false);
    result = read (stream_a, hashables.previous.bytes);
	if (!result)
	{
        result = read (stream_a, hashables.source.bytes);
		if (!result)
		{
            result = read (stream_a, signature.bytes);
            if (!result)
            {
                result = read (stream_a, work);
            }
		}
	}
	return result;
}

bool rai::receive_block::deserialize_json (boost::property_tree::ptree const & tree_a)
{
    auto result (false);
    try
    {
        assert (tree_a.get <std::string> ("type") == "receive");
        auto previous_l (tree_a.get <std::string> ("previous"));
        auto source_l (tree_a.get <std::string> ("source"));
        auto work_l (tree_a.get <std::string> ("work"));
        auto signature_l (tree_a.get <std::string> ("signature"));
        result = hashables.previous.decode_hex (previous_l);
        if (!result)
        {
            result = hashables.source.decode_hex (source_l);
            if (!result)
            {
                result = rai::from_string_hex (work_l, work);
                if (!result)
                {
                    result = signature.decode_hex (signature_l);
                }
            }
        }
    }
    catch (std::runtime_error const &)
    {
        result = true;
    }
    return result;
}

void rai::receive_block::serialize (rai::stream & stream_a) const
{
	write (stream_a, hashables.previous.bytes);
	write (stream_a, hashables.source.bytes);
	write (stream_a, signature.bytes);
    write (stream_a, work);
}

void rai::receive_block::serialize_json (std::string & string_a) const
{
    boost::property_tree::ptree tree;
    tree.put ("type", "receive");
    std::string previous;
    hashables.previous.encode_hex (previous);
    tree.put ("previous", previous);
    std::string source;
    hashables.source.encode_hex (source);
    tree.put ("source", source);
    std::string signature_l;
    signature.encode_hex (signature_l);
    tree.put ("work", rai::to_string_hex (work));
    tree.put ("signature", signature_l);
    std::stringstream ostream;
    boost::property_tree::write_json (ostream, tree);
    string_a = ostream.str ();
}

rai::receive_block::receive_block (rai::block_hash const & previous_a, rai::block_hash const & source_a, rai::private_key const & prv_a, rai::public_key const & pub_a, uint64_t work_a) :
hashables (previous_a, source_a),
signature (rai::sign_message (prv_a, pub_a, hash())),
work (work_a)
{
}

rai::receive_block::receive_block (bool & error_a, rai::stream & stream_a) :
hashables (error_a, stream_a)
{
	if (!error_a)
	{
		error_a = rai::read (stream_a, signature);
		if (!error_a)
		{
			error_a = rai::read (stream_a, work);
		}
	}
}

rai::receive_block::receive_block (bool & error_a, boost::property_tree::ptree const & tree_a) :
hashables (error_a, tree_a)
{
	if (!error_a)
	{
		try
		{
			auto signature_l (tree_a.get <std::string> ("signature"));
			auto work_l (tree_a.get <std::string> ("work"));
			error_a = signature.decode_hex (signature_l);
			if (!error_a)
			{
				error_a = rai::from_string_hex (work_l, work);
			}
		}
		catch (std::runtime_error const &)
		{
			error_a = true;
		}
	}
}

void rai::receive_block::hash (blake2b_state & hash_a) const
{
	hashables.hash (hash_a);
}

uint64_t rai::receive_block::block_work () const
{
    return work;
}

void rai::receive_block::block_work_set (uint64_t work_a)
{
	assert (!rai::work_validate (root (), work_a));
    work = work_a;
}

bool rai::receive_block::operator == (rai::block const & other_a) const
{
    auto other_l (dynamic_cast <rai::receive_block const *> (&other_a));
    auto result (other_l != nullptr);
    if (result)
    {
        result = *this == *other_l;
    }
    return result;
}

rai::block_hash rai::receive_block::previous () const
{
    return hashables.previous;
}

rai::block_hash rai::receive_block::source () const
{
    return hashables.source;
}

rai::block_hash rai::receive_block::root () const
{
	return hashables.previous;
}

std::unique_ptr <rai::block> rai::receive_block::clone () const
{
    return std::unique_ptr <rai::block> (new rai::receive_block (*this));
}

rai::block_type rai::receive_block::type () const
{
    return rai::block_type::receive;
}

rai::receive_hashables::receive_hashables (rai::block_hash const & previous_a, rai::block_hash const & source_a) :
previous (previous_a),
source (source_a)
{
}

rai::receive_hashables::receive_hashables (bool & error_a, rai::stream & stream_a)
{
	error_a = rai::read (stream_a, previous.bytes);
	if (!error_a)
	{
		error_a = rai::read (stream_a, source.bytes);
	}
}

rai::receive_hashables::receive_hashables (bool & error_a, boost::property_tree::ptree const & tree_a)
{
	try
	{
		auto previous_l (tree_a.get <std::string> ("previous"));
		auto source_l (tree_a.get <std::string> ("source"));
		error_a = previous.decode_hex (previous_l);
		if (!error_a)
		{
			error_a = source.decode_hex (source_l);
		}
	}
	catch (std::runtime_error const &)
	{
		error_a = true;
	}
}

void rai::receive_hashables::hash (blake2b_state & hash_a) const
{
	blake2b_update (&hash_a, previous.bytes.data (), sizeof (previous.bytes));
	blake2b_update (&hash_a, source.bytes.data (), sizeof (source.bytes));
}

rai::block_hash rai::block::hash () const
{
    rai::uint256_union result;
    blake2b_state hash_l;
	auto status (blake2b_init (&hash_l, sizeof (result.bytes)));
	assert (status == 0);
    hash (hash_l);
    status = blake2b_final (&hash_l, result.bytes.data (), sizeof (result.bytes));
	assert (status == 0);
    return result;
}

// Serialize a block prefixed with an 8-bit typecode
void rai::serialize_block (rai::stream & stream_a, rai::block const & block_a)
{
    write (stream_a, block_a.type ());
    block_a.serialize (stream_a);
}

uint64_t rai::work_generate (rai::block_hash const & root_a)
{
    xorshift1024star rng;
    rng.s.fill (0x0123456789abcdef);// No seed here, we're not securing anything, s just can't be 0 per the xorshift1024star spec
    uint64_t work;
    blake2b_state hash;
	blake2b_init (&hash, sizeof (work));
    uint64_t output;
    do
    {
        work = rng.next ();
        blake2b_update (&hash, reinterpret_cast <uint8_t *> (&work), sizeof (work));
        blake2b_update (&hash, root_a.bytes.data (), root_a.bytes.size ());
        blake2b_final (&hash, reinterpret_cast <uint8_t *> (&output), sizeof (output));
        blake2b_init (&hash, sizeof (work));
    } while (output < rai::block::publish_threshold);
    return work;
}

void rai::work_generate (rai::block & block_a)
{
    block_a.block_work_set (rai::work_generate (block_a.root ()));
}

bool rai::work_validate (rai::block_hash const & root_a, uint64_t work_a)
{
    uint64_t result;
    blake2b_state hash;
	blake2b_init (&hash, sizeof (result));
    blake2b_update (&hash, reinterpret_cast <uint8_t *> (&work_a), sizeof (work_a));
    blake2b_update (&hash, root_a.bytes.data (), root_a.bytes.size ());
    blake2b_final (&hash, reinterpret_cast <uint8_t *> (&result), sizeof (result));
    return result < rai::block::publish_threshold;
}

bool rai::work_validate (rai::block & block_a)
{
    return rai::work_validate (block_a.root (), block_a.block_work ());
}

std::unique_ptr <rai::block> rai::deserialize_block (rai::stream & stream_a, rai::block_type type_a)
{
    std::unique_ptr <rai::block> result;
    switch (type_a)
    {
        case rai::block_type::receive:
        {
			bool error;
            std::unique_ptr <rai::receive_block> obj (new rai::receive_block (error, stream_a));
            if (!error)
            {
                result = std::move (obj);
            }
            break;
        }
        case rai::block_type::send:
        {
			bool error;
            std::unique_ptr <rai::send_block> obj (new rai::send_block (error, stream_a));
            if (!error)
            {
                result = std::move (obj);
            }
            break;
        }
        case rai::block_type::open:
        {
			bool error;
            std::unique_ptr <rai::open_block> obj (new rai::open_block (error, stream_a));
            if (!error)
            {
                result = std::move (obj);
            }
            break;
        }
        case rai::block_type::change:
        {
            bool error;
            std::unique_ptr <rai::change_block> obj (new rai::change_block (error, stream_a));
            if (!error)
            {
                result = std::move (obj);
            }
            break;
        }
        default:
            break;
    }
    return result;
}

std::unique_ptr <rai::block> rai::deserialize_block_json (boost::property_tree::ptree const & tree_a)
{
    std::unique_ptr <rai::block> result;
    try
    {
        auto type (tree_a.get <std::string> ("type"));
        if (type == "receive")
        {
			bool error;
            std::unique_ptr <rai::receive_block> obj (new rai::receive_block (error, tree_a));
            if (!error)
            {
                result = std::move (obj);
            }
        }
        else if (type == "send")
        {
			bool error;
            std::unique_ptr <rai::send_block> obj (new rai::send_block (error, tree_a));
            if (!error)
            {
                result = std::move (obj);
            }
        }
        else if (type == "open")
        {
			bool error;
            std::unique_ptr <rai::open_block> obj (new rai::open_block (error, tree_a));
            if (!error)
            {
                result = std::move (obj);
            }
        }
        else if (type == "change")
        {
            bool error;
            std::unique_ptr <rai::change_block> obj (new rai::change_block (error, tree_a));
            if (!error)
            {
                result = std::move (obj);
            }
        }
    }
    catch (std::runtime_error const &)
    {
    }
    return result;
}

std::unique_ptr <rai::block> rai::deserialize_block (MDB_val const & val_a)
{
	rai::bufferstream stream (reinterpret_cast <uint8_t const *> (val_a.mv_data), val_a.mv_size);
	return deserialize_block (stream);
}

std::unique_ptr <rai::block> rai::deserialize_block (rai::stream & stream_a)
{
    rai::block_type type;
    auto error (read (stream_a, type));
    std::unique_ptr <rai::block> result;
    if (!error)
    {
         result = rai::deserialize_block (stream_a, type);
    }
    return result;
}

rai::send_block::send_block (rai::account const & destination_a, rai::block_hash const & previous_a, rai::amount const & balance_a, rai::private_key const & prv_a, rai::public_key const & pub_a, uint64_t work_a) :
hashables (destination_a, previous_a, balance_a),
signature (rai::sign_message (prv_a, pub_a, hash ())),
work (work_a)
{
}

rai::send_block::send_block (bool & error_a, rai::stream & stream_a) :
hashables (error_a, stream_a)
{
	if (!error_a)
	{
		error_a = rai::read (stream_a, signature.bytes);
		if (!error_a)
		{
			error_a = rai::read (stream_a, work);
		}
	}
}

rai::send_block::send_block (bool & error_a, boost::property_tree::ptree const & tree_a) :
hashables (error_a, tree_a)
{
	if (!error_a)
	{
		try
		{
			auto signature_l (tree_a.get <std::string> ("signature"));
			auto work_l (tree_a.get <std::string> ("work"));
			error_a = signature.decode_hex (signature_l);
			if (!error_a)
			{
				error_a = rai::from_string_hex (work_l, work);
			}
		}
		catch (std::runtime_error const &)
		{
			error_a = true;
		}
	}
}

bool rai::send_block::operator == (rai::block const & other_a) const
{
    auto other_l (dynamic_cast <rai::send_block const *> (&other_a));
    auto result (other_l != nullptr);
    if (result)
    {
        result = *this == *other_l;
    }
    return result;
}

std::unique_ptr <rai::block> rai::send_block::clone () const
{
    return std::unique_ptr <rai::block> (new rai::send_block (*this));
}

rai::block_type rai::send_block::type () const
{
    return rai::block_type::send;
}

bool rai::send_block::operator == (rai::send_block const & other_a) const
{
    auto result (hashables.destination == other_a.hashables.destination && hashables.previous == other_a.hashables.previous && hashables.balance == other_a.hashables.balance && work == other_a.work && signature == other_a.signature);
    return result;
}

rai::block_hash rai::send_block::previous () const
{
    return hashables.previous;
}

rai::block_hash rai::send_block::source () const
{
    return 0;
}

rai::block_hash rai::send_block::root () const
{
	return hashables.previous;
}

rai::open_hashables::open_hashables (rai::account const & account_a, rai::account const & representative_a, rai::block_hash const & source_a) :
account (account_a),
representative (representative_a),
source (source_a)
{
}

rai::open_hashables::open_hashables (bool & error_a, rai::stream & stream_a)
{
	error_a = rai::read (stream_a, account.bytes);
	if (!error_a)
	{
		error_a = rai::read (stream_a, representative.bytes);
		if (!error_a)
		{
			error_a = rai::read (stream_a, source.bytes);
		}
	}
}

rai::open_hashables::open_hashables (bool & error_a, boost::property_tree::ptree const & tree_a)
{
    try
    {
		auto account_l (tree_a.get <std::string> ("account"));
        auto representative_l (tree_a.get <std::string> ("representative"));
        auto source_l (tree_a.get <std::string> ("source"));
		error_a = account.decode_base58check (account_l);
		if (!error_a)
		{
			error_a = representative.decode_base58check (representative_l);
			if (!error_a)
			{
				error_a = source.decode_hex (source_l);
			}
		}
    }
    catch (std::runtime_error const &)
    {
        error_a = true;
    }
}

void rai::open_hashables::hash (blake2b_state & hash_a) const
{
    blake2b_update (&hash_a, account.bytes.data (), sizeof (account.bytes));
    blake2b_update (&hash_a, representative.bytes.data (), sizeof (representative.bytes));
    blake2b_update (&hash_a, source.bytes.data (), sizeof (source.bytes));
}

rai::open_block::open_block (rai::account const & account_a, rai::account const & representative_a, rai::block_hash const & source_a, rai::private_key const & prv_a, rai::public_key const & pub_a, uint64_t work_a) :
hashables (account_a, representative_a, source_a),
signature (rai::sign_message (prv_a, pub_a, hash ())),
work (work_a)
{
}

rai::open_block::open_block (rai::account const & account_a, rai::account const & representative_a, rai::block_hash const & source_a, std::nullptr_t) :
hashables (account_a, representative_a, source_a),
work (rai::work_generate (root ()))
{
	signature.clear ();
}

rai::open_block::open_block (bool & error_a, rai::stream & stream_a) :
hashables (error_a, stream_a)
{
	if (!error_a)
	{
		error_a = rai::read (stream_a, signature);
		if (!error_a)
		{
			error_a = rai::read (stream_a, work);
		}
	}
}

rai::open_block::open_block (bool & error_a, boost::property_tree::ptree const & tree_a) :
hashables (error_a, tree_a)
{
	if (!error_a)
	{
		try
		{
			auto work_l (tree_a.get <std::string> ("work"));
			auto signature_l (tree_a.get <std::string> ("signature"));
			error_a = rai::from_string_hex (work_l, work);
			if (!error_a)
			{
				error_a = signature.decode_hex (signature_l);
			}
		}
		catch (std::runtime_error const &)
		{
			error_a = true;
		}
	}
}

void rai::open_block::hash (blake2b_state & hash_a) const
{
    hashables.hash (hash_a);
}

uint64_t rai::open_block::block_work () const
{
    return work;
}

void rai::open_block::block_work_set (uint64_t work_a)
{
	assert (!rai::work_validate (root (), work_a));
    work = work_a;
}

rai::block_hash rai::open_block::previous () const
{
    rai::block_hash result (0);
    return result;
}

void rai::open_block::serialize (rai::stream & stream_a) const
{
    write (stream_a, hashables.account);
    write (stream_a, hashables.representative);
    write (stream_a, hashables.source);
    write (stream_a, signature);
    write (stream_a, work);
}

void rai::open_block::serialize_json (std::string & string_a) const
{
    boost::property_tree::ptree tree;
    tree.put ("type", "open");
    tree.put ("account", hashables.account.to_base58check ());
    tree.put ("representative", hashables.representative.to_base58check ());
    tree.put ("source", hashables.source.to_string ());
    std::string signature_l;
    signature.encode_hex (signature_l);
    tree.put ("work", rai::to_string_hex (work));
    tree.put ("signature", signature_l);
    std::stringstream ostream;
    boost::property_tree::write_json (ostream, tree);
    string_a = ostream.str ();
}

bool rai::open_block::deserialize (rai::stream & stream_a)
{
    auto result (read (stream_a, hashables.account));
    if (!result)
    {
        result = read (stream_a, hashables.representative);
        if (!result)
        {
            result = read (stream_a, hashables.source);
            if (!result)
            {
                result = read (stream_a, signature);
                if (!result)
                {
                    result = read (stream_a, work);
                }
            }
        }
    }
    return result;
}

bool rai::open_block::deserialize_json (boost::property_tree::ptree const & tree_a)
{
    auto result (false);
    try
    {
        assert (tree_a.get <std::string> ("type") == "open");
        auto account_l (tree_a.get <std::string> ("account"));
        auto representative_l (tree_a.get <std::string> ("representative"));
        auto source_l (tree_a.get <std::string> ("source"));
        auto work_l (tree_a.get <std::string> ("work"));
        auto signature_l (tree_a.get <std::string> ("signature"));
        result = hashables.account.decode_hex (account_l);
        if (!result)
        {
            result = hashables.representative.decode_hex (representative_l);
            if (!result)
            {
                result = hashables.source.decode_hex (source_l);
                if (!result)
                {
                    result = rai::from_string_hex (work_l, work);
                    if (!result)
                    {
                        result = signature.decode_hex (signature_l);
                    }
                }
            }
        }
    }
    catch (std::runtime_error const &)
    {
        result = true;
    }
    return result;
}

void rai::open_block::visit (rai::block_visitor & visitor_a) const
{
    visitor_a.open_block (*this);
}

std::unique_ptr <rai::block> rai::open_block::clone () const
{
    return std::unique_ptr <rai::block> (new rai::open_block (*this));
}

rai::block_type rai::open_block::type () const
{
    return rai::block_type::open;
}

bool rai::open_block::operator == (rai::block const & other_a) const
{
    auto other_l (dynamic_cast <rai::open_block const *> (&other_a));
    auto result (other_l != nullptr);
    if (result)
    {
        result = *this == *other_l;
    }
    return result;
}

bool rai::open_block::operator == (rai::open_block const & other_a) const
{
    return hashables.account == other_a.hashables.account && hashables.representative == other_a.hashables.representative && hashables.source == other_a.hashables.source && work == other_a.work && signature == other_a.signature;
}

rai::block_hash rai::open_block::source () const
{
    return hashables.source;
}

rai::block_hash rai::open_block::root () const
{
	return hashables.account;
}

rai::change_hashables::change_hashables (rai::account const & representative_a, rai::block_hash const & previous_a) :
representative (representative_a),
previous (previous_a)
{
}

rai::change_hashables::change_hashables (bool & error_a, rai::stream & stream_a)
{
    error_a = rai::read (stream_a, representative);
    if (!error_a)
    {
        error_a = rai::read (stream_a, previous);
    }
}

rai::change_hashables::change_hashables (bool & error_a, boost::property_tree::ptree const & tree_a)
{
    try
    {
        auto representative_l (tree_a.get <std::string> ("representative"));
        auto previous_l (tree_a.get <std::string> ("previous"));
        error_a = representative.decode_base58check (representative_l);
        if (!error_a)
        {
            error_a = previous.decode_hex (previous_l);
        }
    }
    catch (std::runtime_error const &)
    {
        error_a = true;
    }
}

void rai::change_hashables::hash (blake2b_state & hash_a) const
{
    blake2b_update (&hash_a, representative.bytes.data (), sizeof (representative.bytes));
    blake2b_update (&hash_a, previous.bytes.data (), sizeof (previous.bytes));
}

rai::change_block::change_block (rai::account const & representative_a, rai::block_hash const & previous_a, rai::private_key const & prv_a, rai::public_key const & pub_a, uint64_t work_a) :
hashables (representative_a, previous_a),
signature (rai::sign_message (prv_a, pub_a, hash ())),
work (work_a)
{
}

rai::change_block::change_block (bool & error_a, rai::stream & stream_a) :
hashables (error_a, stream_a)
{
    if (!error_a)
    {
        error_a = rai::read (stream_a, signature);
        if (!error_a)
        {
            error_a = rai::read (stream_a, work);
        }
    }
}

rai::change_block::change_block (bool & error_a, boost::property_tree::ptree const & tree_a) :
hashables (error_a, tree_a)
{
    if (!error_a)
    {
        try
        {
            auto work_l (tree_a.get <std::string> ("work"));
            auto signature_l (tree_a.get <std::string> ("signature"));
            error_a = rai::from_string_hex (work_l, work);
            if (!error_a)
            {
                error_a = signature.decode_hex (signature_l);
            }
        }
        catch (std::runtime_error const &)
        {
            error_a = true;
        }
    }
}

void rai::change_block::hash (blake2b_state & hash_a) const
{
    hashables.hash (hash_a);
}

uint64_t rai::change_block::block_work () const
{
    return work;
}

void rai::change_block::block_work_set (uint64_t work_a)
{
	assert (!rai::work_validate (root (), work_a));
    work = work_a;
}

rai::block_hash rai::change_block::previous () const
{
    return hashables.previous;
}

void rai::change_block::serialize (rai::stream & stream_a) const
{
    write (stream_a, hashables.representative);
    write (stream_a, hashables.previous);
    write (stream_a, signature);
    write (stream_a, work);
}

void rai::change_block::serialize_json (std::string & string_a) const
{
    boost::property_tree::ptree tree;
    tree.put ("type", "change");
    tree.put ("representative", hashables.representative.to_base58check ());
    tree.put ("previous", hashables.previous.to_string ());
    tree.put ("work", rai::to_string_hex (work));
    std::string signature_l;
    signature.encode_hex (signature_l);
    tree.put ("signature", signature_l);
    std::stringstream ostream;
    boost::property_tree::write_json (ostream, tree);
    string_a = ostream.str ();
}

bool rai::change_block::deserialize (rai::stream & stream_a)
{
    auto result (read (stream_a, hashables.representative));
    if (!result)
    {
        result = read (stream_a, hashables.previous);
        if (!result)
        {
            result = read (stream_a, signature);
            if (!result)
            {
                result = read (stream_a, work);
            }
        }
    }
    return result;
}

bool rai::change_block::deserialize_json (boost::property_tree::ptree const & tree_a)
{
    auto result (false);
    try
    {
        assert (tree_a.get <std::string> ("type") == "change");
        auto representative_l (tree_a.get <std::string> ("representative"));
        auto previous_l (tree_a.get <std::string> ("previous"));
        auto work_l (tree_a.get <std::string> ("work"));
        auto signature_l (tree_a.get <std::string> ("signature"));
        result = hashables.representative.decode_hex (representative_l);
        if (!result)
        {
            result = hashables.previous.decode_hex (previous_l);
            if (!result)
            {
                result = rai::from_string_hex (work_l, work);
                if (!result)
                {
                    result = signature.decode_hex (signature_l);
                }
            }
        }
    }
    catch (std::runtime_error const &)
    {
        result = true;
    }
    return result;
}

void rai::change_block::visit (rai::block_visitor & visitor_a) const
{
    visitor_a.change_block (*this);
}

std::unique_ptr <rai::block> rai::change_block::clone () const
{
    return std::unique_ptr <rai::block> (new rai::change_block (*this));
}

rai::block_type rai::change_block::type () const
{
    return rai::block_type::change;
}

bool rai::change_block::operator == (rai::block const & other_a) const
{
    auto other_l (dynamic_cast <rai::change_block const *> (&other_a));
    auto result (other_l != nullptr);
    if (result)
    {
        result = *this == *other_l;
    }
    return result;
}

bool rai::change_block::operator == (rai::change_block const & other_a) const
{
    return hashables.representative == other_a.hashables.representative && hashables.previous == other_a.hashables.previous && work == other_a.work && signature == other_a.signature;
}

rai::block_hash rai::change_block::source () const
{
    return 0;
}

rai::block_hash rai::change_block::root () const
{
	return hashables.previous;
}

rai::frontier::frontier () :
hash (0),
representative (0),
balance (0),
time (0)
{
}

rai::frontier::frontier (MDB_val const & val_a)
{
	static_assert (sizeof (hash) + sizeof (representative) + sizeof (balance) + sizeof (time) == sizeof (*this), "Class not packed");
	std::copy (reinterpret_cast <uint8_t const *> (val_a.mv_data), reinterpret_cast <uint8_t const *> (val_a.mv_data) + val_a.mv_size, reinterpret_cast <uint8_t *> (this));
}

rai::frontier::frontier (rai::block_hash const & hash_a, rai::account const & representative_a, rai::amount const & balance_a, uint64_t time_a) :
hash (hash_a),
representative (representative_a),
balance (balance_a),
time (time_a)
{
}

void rai::frontier::serialize (rai::stream & stream_a) const
{
    write (stream_a, hash.bytes);
    write (stream_a, representative.bytes);
    write (stream_a, balance.bytes);
    write (stream_a, time);
}

bool rai::frontier::deserialize (rai::stream & stream_a)
{
    auto result (read (stream_a, hash.bytes));
    if (!result)
    {
        result = read (stream_a, representative.bytes);
        if (!result)
        {
            result = read (stream_a, balance.bytes);
            if (!result)
            {
                result = read (stream_a, time);
            }
        }
    }
    return result;
}

bool rai::frontier::operator == (rai::frontier const & other_a) const
{
    return hash == other_a.hash && representative == other_a.representative && balance == other_a.balance && time == other_a.time;
}

bool rai::frontier::operator != (rai::frontier const & other_a) const
{
    return ! (*this == other_a);
}

rai::mdb_val rai::frontier::val () const
{
	return rai::mdb_val (sizeof (*this), const_cast <rai::frontier *> (this));
}

rai::store_entry::store_entry ()
{
	clear ();
}

void rai::store_entry::clear ()
{
	first = {0, nullptr};
	second = {0, nullptr};
}

rai::store_entry * rai::store_entry::operator -> ()
{
    return this;
}

rai::store_entry & rai::store_iterator::operator -> ()
{
    return current;
}

rai::store_iterator::store_iterator (MDB_txn * transaction_a, MDB_dbi db_a) :
cursor (nullptr)
{
	auto status (mdb_cursor_open (transaction_a, db_a, &cursor));
	assert (status == 0);
	auto status2 (mdb_cursor_get (cursor, &current.first, &current.second, MDB_FIRST));
	assert (status2 == 0 || status2 == MDB_NOTFOUND);
	if (status2 != MDB_NOTFOUND)
	{
		auto status3 (mdb_cursor_get (cursor, &current.first, &current.second, MDB_GET_CURRENT));
		assert (status3 == 0 || status3 == MDB_NOTFOUND);
	}
	else
	{
		current.clear ();
	}
}

rai::store_iterator::store_iterator (std::nullptr_t) :
cursor (nullptr)
{
}

rai::store_iterator::store_iterator (MDB_txn * transaction_a, MDB_dbi db_a, MDB_val const & val_a) :
cursor (nullptr)
{
	auto status (mdb_cursor_open (transaction_a, db_a, &cursor));
	assert (status == 0);
	current.first = val_a;
	auto status2 (mdb_cursor_get (cursor, &current.first, &current.second, MDB_SET_RANGE));
	assert (status2 == 0 || status2 == MDB_NOTFOUND);
	if (status2 != MDB_NOTFOUND)
	{
		auto status3 (mdb_cursor_get (cursor, &current.first, &current.second, MDB_GET_CURRENT));
		assert (status3 == 0 || status3 == MDB_NOTFOUND);
	}
	else
	{
		current.clear ();
	}
}

rai::store_iterator::store_iterator (rai::store_iterator && other_a)
{
	cursor = other_a.cursor;
	other_a.cursor = nullptr;
	current = other_a.current;
}

rai::store_iterator::~store_iterator ()
{
	if (cursor != nullptr)
	{
		mdb_cursor_close (cursor);
	}
}

rai::store_iterator & rai::store_iterator::operator ++ ()
{
	assert (cursor != nullptr);
	auto status (mdb_cursor_get (cursor, &current.first, &current.second, MDB_NEXT));
	if (status == MDB_NOTFOUND)
	{
		current.clear ();
	}
    return *this;
}

rai::store_iterator & rai::store_iterator::operator = (rai::store_iterator && other_a)
{
	if (cursor != nullptr)
	{
		mdb_cursor_close (cursor);
	}
	cursor = other_a.cursor;
	other_a.cursor = nullptr;
	current = other_a.current;
	other_a.current.clear ();
	return *this;
}

bool rai::store_iterator::operator == (rai::store_iterator const & other_a) const
{
	auto result (current.first.mv_data == other_a.current.first.mv_data);
	assert (!result || (current.first.mv_size == other_a.current.first.mv_size));
	assert (!result || (current.second.mv_data == other_a.current.second.mv_data));
	assert (!result || (current.second.mv_size == other_a.current.second.mv_size));
	return result;
}

bool rai::store_iterator::operator != (rai::store_iterator const & other_a) const
{
    return !(*this == other_a);
}

rai::block_store::block_store (bool & error_a, boost::filesystem::path const & path_a) :
environment (error_a, path_a),
accounts (0),
blocks (0),
pending (0),
representation (0),
unchecked (0),
unsynced (0),
stack (0),
checksum (0)
{
	if (!error_a)
	{
		rai::transaction transaction (environment, nullptr, true);
		error_a = error_a || mdb_dbi_open (transaction, "accounts", MDB_CREATE, &accounts) != 0;
		error_a = error_a || mdb_dbi_open (transaction, "blocks", MDB_CREATE, &blocks) != 0;
		error_a = error_a || mdb_dbi_open (transaction, "pending", MDB_CREATE, &pending) != 0;
		error_a = error_a || mdb_dbi_open (transaction, "representation", MDB_CREATE, &representation) != 0;
		error_a = error_a || mdb_dbi_open (transaction, "unchecked", MDB_CREATE, &unchecked) != 0;
		error_a = error_a || mdb_dbi_open (transaction, "unsynced", MDB_CREATE, &unsynced) != 0;
		error_a = error_a || mdb_dbi_open (transaction, "stack", MDB_CREATE, &stack) != 0;
		error_a = error_a || mdb_dbi_open (transaction, "checksum", MDB_CREATE, &checksum) != 0;
		if (!error_a)
		{
			checksum_put (transaction, 0, 0, 0);
		}
	}
}

void rai::block_store::clear (MDB_dbi db_a)
{
	rai::transaction transaction (environment, nullptr, true);
	auto status (mdb_drop (transaction, db_a, 0));
	assert (status == 0);
}

namespace
{
// Fill in our predecessors
class set_predecessor : public rai::block_visitor
{
public:
	set_predecessor (MDB_txn * transaction_a, rai::block_store & store_a) :
	transaction (transaction_a),
	store (store_a)
	{
	}
	void fill_value (rai::block const & block_a)
	{
		auto hash (block_a.hash ());
		auto value (store.block_get_raw (transaction, block_a.previous ()));
		assert (value.mv_size != 0);
		std::vector <uint8_t> data (static_cast <uint8_t *> (value.mv_data), static_cast <uint8_t *> (value.mv_data) + value.mv_size);
		std::copy (hash.bytes.begin (), hash.bytes.end (), data.end () - hash.bytes.size ());
		store.block_put_raw (transaction, block_a.previous (), rai::mdb_val (data.size (), data.data()));
	}
	void send_block (rai::send_block const & block_a) override
	{
		fill_value (block_a);
	}
	void receive_block (rai::receive_block const & block_a) override
	{
		fill_value (block_a);
	}
	void open_block (rai::open_block const & block_a) override
	{
		// Open blocks don't have a predecessor
	}
	void change_block (rai::change_block const & block_a) override
	{
		fill_value (block_a);
	}
	MDB_txn * transaction;
	rai::block_store & store;
};
}

void rai::block_store::block_put_raw (MDB_txn * transaction_a, rai::block_hash const & hash_a, MDB_val value_a)
{
    auto status2 (mdb_put (transaction_a, blocks, hash_a.val (), &value_a, 0));
	assert (status2 == 0);
}

void rai::block_store::block_put (MDB_txn * transaction_a, rai::block_hash const & hash_a, rai::block const & block_a)
{
    std::vector <uint8_t> vector;
    {
        rai::vectorstream stream (vector);
        rai::serialize_block (stream, block_a);
		rai::block_hash successor (0);
		rai::write (stream, successor.bytes);
    }
	block_put_raw (transaction_a, hash_a, {vector.size (), vector.data ()});
	set_predecessor predecessor (transaction_a, *this);
	block_a.visit (predecessor);
	assert (block_a.previous ().is_zero () || block_successor (transaction_a, block_a.previous ()) == hash_a);
}

MDB_val rai::block_store::block_get_raw (MDB_txn * transaction_a, rai::block_hash const & hash_a)
{
	MDB_val result {0, nullptr};
	auto status (mdb_get (transaction_a, blocks, hash_a.val (), &result));
	assert (status == 0 || status == MDB_NOTFOUND);
	return result;
}

rai::block_hash rai::block_store::block_successor (MDB_txn * transaction_a, rai::block_hash const & hash_a)
{
	auto value (block_get_raw (transaction_a, hash_a));
	rai::block_hash result;
	if (value.mv_size != 0)
	{
		assert (value.mv_size >= result.bytes.size ());
		rai::bufferstream stream (reinterpret_cast <uint8_t const *> (value.mv_data) + value.mv_size - result.bytes.size (), result.bytes.size ());
		auto error (rai::read (stream, result.bytes));
		assert (!error);
	}
	else
	{
		result.clear ();
	}
	return result;
}

std::unique_ptr <rai::block> rai::block_store::block_get (MDB_txn * transaction_a, rai::block_hash const & hash_a)
{
	auto value (block_get_raw (transaction_a, hash_a));
    std::unique_ptr <rai::block> result;
    if (value.mv_size != 0)
    {
        rai::bufferstream stream (reinterpret_cast <uint8_t const *> (value.mv_data), value.mv_size);
        result = rai::deserialize_block (stream);
        assert (result != nullptr);
    }
    return result;
}

void rai::block_store::block_del (MDB_txn * transaction_a, rai::block_hash const & hash_a)
{
	auto status (mdb_del (transaction_a, blocks, hash_a.val (), nullptr));
    assert (status == 0);
}

bool rai::block_store::block_exists (MDB_txn * transaction_a, rai::block_hash const & hash_a)
{
	auto iterator (blocks_begin (transaction_a, hash_a));
	return iterator->second.mv_size != 0 && hash_a == rai::block_hash (iterator->first);
}

void rai::block_store::latest_del (MDB_txn * transaction_a, rai::account const & account_a)
{
	auto status (mdb_del (transaction_a, accounts, account_a.val (), nullptr));
    assert (status == 0);
}

bool rai::block_store::latest_exists (rai::account const & account_a)
{
	rai::transaction transaction (environment, nullptr, false);
	auto iterator (latest_begin (transaction, account_a));
	return rai::account (iterator->first) == account_a;
}

bool rai::block_store::latest_get (MDB_txn * transaction_a, rai::account const & account_a, rai::frontier & frontier_a)
{
	MDB_val value;
	auto status (mdb_get (transaction_a, accounts, account_a.val (), &value));
	assert (status == 0 || status == MDB_NOTFOUND);
    bool result;
    if (status == MDB_NOTFOUND)
    {
        result = true;
    }
    else
    {
        rai::bufferstream stream (reinterpret_cast <uint8_t const *> (value.mv_data), value.mv_size);
        result = frontier_a.deserialize (stream);
        assert (!result);
    }
    return result;
}

void rai::block_store::latest_put (MDB_txn * transaction_a, rai::account const & account_a, rai::frontier const & frontier_a)
{
    std::vector <uint8_t> vector;
    {
        rai::vectorstream stream (vector);
        frontier_a.serialize (stream);
    }
	auto status (mdb_put (transaction_a, accounts, account_a.val (), frontier_a.val (), 0));
    assert (status == 0);
}

void rai::block_store::pending_put (MDB_txn * transaction_a, rai::block_hash const & hash_a, rai::receivable const & receivable_a)
{
    std::vector <uint8_t> vector;
    {
        rai::vectorstream stream (vector);
        rai::write (stream, receivable_a.source);
        rai::write (stream, receivable_a.amount);
        rai::write (stream, receivable_a.destination);
    }
	auto status (mdb_put (transaction_a, pending, hash_a.val (), receivable_a.val (), 0));
    assert (status == 0);
}

void rai::block_store::pending_del (MDB_txn * transaction_a, rai::block_hash const & hash_a)
{
	auto status (mdb_del (transaction_a, pending, hash_a.val (), nullptr));
    assert (status == 0);
}

bool rai::block_store::pending_exists (MDB_txn * transaction_a, rai::block_hash const & hash_a)
{
	auto iterator (pending_begin (transaction_a, hash_a));
    return rai::block_hash (iterator->first) == hash_a;
}

bool rai::block_store::pending_get (MDB_txn * transaction_a, rai::block_hash const & hash_a, rai::receivable & receivable_a)
{
	MDB_val value;
	auto status (mdb_get (transaction_a, pending, hash_a.val (), &value));
	assert (status == 0 || status == MDB_NOTFOUND);
    bool result;
    if (status == MDB_NOTFOUND)
    {
        result = true;
    }
    else
    {
        result = false;
        assert (value.mv_size == sizeof (receivable_a.source.bytes) + sizeof (receivable_a.amount.bytes) + sizeof (receivable_a.destination.bytes));
        rai::bufferstream stream (reinterpret_cast <uint8_t const *> (value.mv_data), value.mv_size);
        auto error1 (rai::read (stream, receivable_a.source));
        assert (!error1);
        auto error2 (rai::read (stream, receivable_a.amount));
        assert (!error2);
        auto error3 (rai::read (stream, receivable_a.destination));
        assert (!error3);
    }
    return result;
}

rai::store_iterator rai::block_store::pending_begin (MDB_txn * transaction_a, rai::block_hash const & hash_a)
{
	rai::store_iterator result (transaction_a, pending, hash_a.val ());
	return result;
}

rai::store_iterator rai::block_store::pending_begin (MDB_txn * transaction_a)
{
    rai::store_iterator result (transaction_a, pending);
    return result;
}

rai::store_iterator rai::block_store::pending_end ()
{
    rai::store_iterator result (nullptr);
    return result;
}

rai::receivable::receivable () :
source (0),
amount (0),
destination (0)
{
}

rai::receivable::receivable (MDB_val const & val_a)
{
	static_assert (sizeof (source) + sizeof (amount) + sizeof (destination) == sizeof (*this), "Packed class");
	std::copy (reinterpret_cast <uint8_t const *> (val_a.mv_data), reinterpret_cast <uint8_t const *> (val_a.mv_data) + val_a.mv_size, reinterpret_cast <uint8_t *> (this));
}

rai::receivable::receivable (rai::account const & source_a, rai::amount const & amount_a, rai::account const & destination_a) :
source (source_a),
amount (amount_a),
destination (destination_a)
{
}

void rai::receivable::serialize (rai::stream & stream_a) const
{
    rai::write (stream_a, source.bytes);
    rai::write (stream_a, amount.bytes);
    rai::write (stream_a, destination.bytes);
}

bool rai::receivable::deserialize (rai::stream & stream_a)
{
    auto result (rai::read (stream_a, source.bytes));
    if (!result)
    {
        result = rai::read (stream_a, amount.bytes);
        if (!result)
        {
            result = rai::read (stream_a, destination.bytes);
        }
    }
    return result;
}

bool rai::receivable::operator == (rai::receivable const & other_a) const
{
    return source == other_a.source && amount == other_a.amount && destination == other_a.destination;
}

rai::mdb_val rai::receivable::val () const
{
	return rai::mdb_val (sizeof (*this), const_cast <rai::receivable *> (this));
}

rai::uint128_t rai::block_store::representation_get (MDB_txn * transaction_a, rai::account const & account_a)
{
	MDB_val value;
	auto status (mdb_get (transaction_a, representation, account_a.val (), &value));
	assert (status == 0 || status == MDB_NOTFOUND);
    rai::uint128_t result;
    if (status == 0)
    {
        rai::uint128_union rep;
        rai::bufferstream stream (reinterpret_cast <uint8_t const *> (value.mv_data), value.mv_size);
        auto error (rai::read (stream, rep));
        assert (!error);
        result = rep.number ();
    }
    else
    {
        result = 0;
    }
    return result;
}

void rai::block_store::representation_put (MDB_txn * transaction_a, rai::account const & account_a, rai::uint128_t const & representation_a)
{
    rai::uint128_union rep (representation_a);
	auto status (mdb_put (transaction_a, representation, account_a.val (), rep.val (), 0));
    assert (status == 0);
}

void rai::block_store::unchecked_put (MDB_txn * transaction_a, rai::block_hash const & hash_a, rai::block const & block_a)
{
    std::vector <uint8_t> vector;
    {
        rai::vectorstream stream (vector);
        rai::serialize_block (stream, block_a);
    }
	auto status (mdb_put (transaction_a, unchecked, hash_a.val (), rai::mdb_val (vector.size (), vector.data ()), 0));
	assert (status == 0);
}

std::unique_ptr <rai::block> rai::block_store::unchecked_get (MDB_txn * transaction_a, rai::block_hash const & hash_a)
{
	MDB_val value;
	auto status (mdb_get (transaction_a, unchecked, hash_a.val (), &value));
	assert (status == 0 || status == MDB_NOTFOUND);
    std::unique_ptr <rai::block> result;
    if (status == 0)
    {
        rai::bufferstream stream (reinterpret_cast <uint8_t const *> (value.mv_data), value.mv_size);
        result = rai::deserialize_block (stream);
        assert (result != nullptr);
    }
    return result;
}

void rai::block_store::unchecked_del (MDB_txn * transaction_a, rai::block_hash const & hash_a)
{
	auto status (mdb_del (transaction_a, unchecked, hash_a.val (), nullptr));
	assert (status == 0 || status == MDB_NOTFOUND);
}

rai::store_iterator rai::block_store::unchecked_begin (MDB_txn * transaction_a)
{
    rai::store_iterator result (transaction_a, unchecked);
    return result;
}

rai::store_iterator rai::block_store::unchecked_end ()
{
    rai::store_iterator result (nullptr);
    return result;
}

void rai::block_store::unsynced_put (MDB_txn * transaction_a, rai::block_hash const & hash_a)
{
	auto status (mdb_put (transaction_a, unsynced, hash_a.val (), rai::mdb_val (0, nullptr), 0));
	assert (status == 0);
}

void rai::block_store::unsynced_del (MDB_txn * transaction_a, rai::block_hash const & hash_a)
{
	auto status (mdb_del (transaction_a, unsynced, hash_a.val (), nullptr));
	assert (status == 0);
}

bool rai::block_store::unsynced_exists (MDB_txn * transaction_a, rai::block_hash const & hash_a)
{
	auto iterator (unsynced_begin (transaction_a, hash_a));
	return rai::block_hash (iterator->first) == hash_a;
}

rai::store_iterator rai::block_store::unsynced_begin (MDB_txn * transaction_a)
{
    return rai::store_iterator (transaction_a, unsynced);
}

rai::store_iterator rai::block_store::unsynced_begin (MDB_txn * transaction_a, rai::uint256_union const & val_a)
{
	return rai::store_iterator (transaction_a, unsynced, val_a.val ());
}

rai::store_iterator rai::block_store::unsynced_end ()
{
    return rai::store_iterator (nullptr);
}

void rai::block_store::stack_push (uint64_t key_a, rai::block_hash const & hash_a)
{
	rai::transaction transaction (environment, nullptr, true);
	auto status (mdb_put (transaction, stack, rai::mdb_val (sizeof (key_a), &key_a), hash_a.val (), 0));
	assert (status == 0);
}

rai::block_hash rai::block_store::stack_pop (uint64_t key_a)
{
	rai::transaction transaction (environment, nullptr, true);
	MDB_val value;
	auto status (mdb_get (transaction, stack, rai::mdb_val (sizeof (key_a), &key_a), &value));
	assert (status == 0);
	rai::block_hash result;
	assert (value.mv_size == result.chars.size ());
    std::copy (reinterpret_cast <uint8_t const *> (value.mv_data), reinterpret_cast <uint8_t const *> (value.mv_data) + value.mv_size, result.chars.data ());
	auto status2 (mdb_del (transaction, stack, rai::mdb_val (sizeof (key_a), &key_a), nullptr));
	assert (status2 == 0);
    return result;
}

void rai::block_store::checksum_put (MDB_txn * transaction_a, uint64_t prefix, uint8_t mask, rai::uint256_union const & hash_a)
{
    assert ((prefix & 0xff) == 0);
    uint64_t key (prefix | mask);
	auto status (mdb_put (transaction_a, checksum, rai::mdb_val (sizeof (key), &key), hash_a.val (), 0));
	assert (status == 0);
}

bool rai::block_store::checksum_get (MDB_txn * transaction_a, uint64_t prefix, uint8_t mask, rai::uint256_union & hash_a)
{
    assert ((prefix & 0xff) == 0);
    uint64_t key (prefix | mask);
	MDB_val value;
	auto status (mdb_get (transaction_a, checksum, rai::mdb_val (sizeof (key), &key), &value));
	assert (status == 0 || status == MDB_NOTFOUND);
    bool result;
    if (status == 0)
    {
        result = false;
        rai::bufferstream stream (reinterpret_cast <uint8_t const *> (value.mv_data), value.mv_size);
        auto error (rai::read (stream, hash_a));
        assert (!error);
    }
    else
    {
        result = true;
    }
    return result;
}

void rai::block_store::checksum_del (MDB_txn * transaction_a, uint64_t prefix, uint8_t mask)
{
    assert ((prefix & 0xff) == 0);
    uint64_t key (prefix | mask);
	auto status (mdb_del (transaction_a, checksum, rai::mdb_val (sizeof (key), &key), nullptr));
	assert (status == 0);
}

namespace
{
class root_visitor : public rai::block_visitor
{
public:
    root_visitor (rai::block_store & store_a) :
    store (store_a)
    {
    }
    void send_block (rai::send_block const & block_a) override
    {
        result = block_a.previous ();
    }
    void receive_block (rai::receive_block const & block_a) override
    {
        result = block_a.previous ();
    }
    // Open blocks have no previous () so we use the account number
    void open_block (rai::open_block const & block_a) override
    {
		rai::transaction transaction (store.environment, nullptr, false);
        auto hash (block_a.source ());
        auto source (store.block_get (transaction, hash));
        if (source != nullptr)
		{
			auto send (dynamic_cast <rai::send_block *> (source.get ()));
			if (send != nullptr)
			{
				result = send->hashables.destination;
			}
			else
			{
				result.clear ();
			}
		}
		else
		{
			result.clear ();
		}
    }
    void change_block (rai::change_block const & block_a) override
    {
        result = block_a.previous ();
    }
    rai::block_store & store;
    rai::block_hash result;
};
}

rai::store_iterator rai::block_store::blocks_begin (MDB_txn * transaction_a, rai::uint256_union const & hash_a)
{
	rai::store_iterator result (transaction_a, blocks, hash_a.val ());
	return result;
}

rai::store_iterator rai::block_store::blocks_begin (MDB_txn * transaction_a)
{
    rai::store_iterator result (transaction_a, blocks);
    return result;
}

rai::store_iterator rai::block_store::blocks_end ()
{
    rai::store_iterator result (nullptr);
    return result;
}

rai::store_iterator rai::block_store::latest_begin (MDB_txn * transaction_a, rai::account const & account_a)
{
    rai::store_iterator result (transaction_a, accounts, account_a.val ());
    return result;
}

rai::store_iterator rai::block_store::latest_begin (MDB_txn * transaction_a)
{
    rai::store_iterator result (transaction_a, accounts);
    return result;
}

rai::store_iterator rai::block_store::latest_end ()
{
    rai::store_iterator result (nullptr);
    return result;
}

namespace
{
class ledger_processor : public rai::block_visitor
{
public:
    ledger_processor (rai::ledger &, MDB_txn *);
    void send_block (rai::send_block const &) override;
    void receive_block (rai::receive_block const &) override;
    void open_block (rai::open_block const &) override;
    void change_block (rai::change_block const &) override;
    rai::ledger & ledger;
	MDB_txn * transaction;
    rai::process_result result;
};

// Determine the amount delta resultant from this block
class amount_visitor : public rai::block_visitor
{
public:
    amount_visitor (MDB_txn *, rai::block_store &);
    void compute (rai::block_hash const &);
    void send_block (rai::send_block const &) override;
    void receive_block (rai::receive_block const &) override;
    void open_block (rai::open_block const &) override;
    void change_block (rai::change_block const &) override;
    void from_send (rai::block_hash const &);
	MDB_txn * transaction;
    rai::block_store & store;
    rai::uint128_t result;
};

// Determine the balance as of this block
class balance_visitor : public rai::block_visitor
{
public:
    balance_visitor (MDB_txn *, rai::block_store &);
    void compute (rai::block_hash const &);
    void send_block (rai::send_block const &) override;
    void receive_block (rai::receive_block const &) override;
    void open_block (rai::open_block const &) override;
    void change_block (rai::change_block const &) override;
	MDB_txn * transaction;
    rai::block_store & store;
    rai::uint128_t result;
};

// Determine the account for this block
class account_visitor : public rai::block_visitor
{
public:
    account_visitor (MDB_txn * transaction_a, rai::block_store & store_a) :
    store (store_a),
	transaction (transaction_a)
    {
    }
    void compute (rai::block_hash const & hash_block)
    {
        auto block (store.block_get (transaction, hash_block));
        assert (block != nullptr);
        block->visit (*this);
    }
    void send_block (rai::send_block const & block_a) override
    {
        account_visitor prev (transaction, store);
        prev.compute (block_a.hashables.previous);
        result = prev.result;
    }
    void receive_block (rai::receive_block const & block_a) override
    {
        auto block (store.block_get (transaction, block_a.hashables.source));
        assert (dynamic_cast <rai::send_block *> (block.get ()) != nullptr);
        auto send (static_cast <rai::send_block *> (block.get ()));
        result = send->hashables.destination;
    }
    void open_block (rai::open_block const & block_a) override
    {
        result = block_a.hashables.account;
    }
    void change_block (rai::change_block const & block_a) override
    {
        account_visitor prev (transaction, store);
        prev.compute (block_a.hashables.previous);
        result = prev.result;
    }
    rai::block_store & store;
	MDB_txn * transaction;
    rai::account result;
};

amount_visitor::amount_visitor (MDB_txn * transaction_a, rai::block_store & store_a) :
transaction (transaction_a),
store (store_a)
{
}

void amount_visitor::send_block (rai::send_block const & block_a)
{
    balance_visitor prev (transaction, store);
    prev.compute (block_a.hashables.previous);
    result = prev.result - block_a.hashables.balance.number ();
}

void amount_visitor::receive_block (rai::receive_block const & block_a)
{
    from_send (block_a.hashables.source);
}

void amount_visitor::open_block (rai::open_block const & block_a)
{
    from_send (block_a.hashables.source);
}

void amount_visitor::change_block (rai::change_block const & block_a)
{
    assert (false);
}

void amount_visitor::from_send (rai::block_hash const & hash_a)
{
    balance_visitor source (transaction, store);
    source.compute (hash_a);
    auto source_block (store.block_get (transaction, hash_a));
    assert (source_block != nullptr);
    balance_visitor source_prev (transaction, store);
    source_prev.compute (source_block->previous ());
}

balance_visitor::balance_visitor (MDB_txn * transaction_a, rai::block_store & store_a) :
transaction (transaction_a),
store (store_a),
result (0)
{
}

void balance_visitor::send_block (rai::send_block const & block_a)
{
    result = block_a.hashables.balance.number ();
}

void balance_visitor::receive_block (rai::receive_block const & block_a)
{
    balance_visitor prev (transaction, store);
    prev.compute (block_a.hashables.previous);
    amount_visitor source (transaction, store);
    source.compute (block_a.hashables.source);
    result = prev.result + source.result;
}

void balance_visitor::open_block (rai::open_block const & block_a)
{
    amount_visitor source (transaction, store);
    source.compute (block_a.hashables.source);
    result = source.result;
}

void balance_visitor::change_block (rai::change_block const & block_a)
{
    balance_visitor prev (transaction, store);
    prev.compute (block_a.hashables.previous);
    result = prev.result;
}

// Determine the representative for this block
class representative_visitor : public rai::block_visitor
{
public:
    representative_visitor (MDB_txn * transaction_a, rai::block_store & store_a) :
	transaction (transaction_a),
    store (store_a)
    {
    }
    void compute (rai::block_hash const & hash_a)
    {
        auto block (store.block_get (transaction, hash_a));
        assert (block != nullptr);
        block->visit (*this);
    }
    void send_block (rai::send_block const & block_a) override
    {
        representative_visitor visitor (transaction, store);
        visitor.compute (block_a.previous ());
        result = visitor.result;
    }
    void receive_block (rai::receive_block const & block_a) override
    {
        representative_visitor visitor (transaction, store);
        visitor.compute (block_a.previous ());
        result = visitor.result;
    }
    void open_block (rai::open_block const & block_a) override
    {
        result = block_a.hashables.representative;
    }
    void change_block (rai::change_block const & block_a) override
    {
        result = block_a.hashables.representative;
    }
	MDB_txn * transaction;
    rai::block_store & store;
    rai::account result;
};

// Rollback this block
class rollback_visitor : public rai::block_visitor
{
public:
    rollback_visitor (MDB_txn * transaction_a, rai::ledger & ledger_a) :
	transaction (transaction_a),
    ledger (ledger_a)
    {
    }
    void send_block (rai::send_block const & block_a) override
    {
        auto hash (block_a.hash ());
        rai::receivable receivable;
        while (ledger.store.pending_get (transaction, hash, receivable))
        {
            ledger.rollback (transaction, ledger.latest (transaction, block_a.hashables.destination));
        }
        rai::frontier frontier;
        ledger.store.latest_get (transaction, receivable.source, frontier);
        ledger.store.pending_del (transaction, hash);
        ledger.change_latest (transaction, receivable.source, block_a.hashables.previous, frontier.representative, ledger.balance (transaction, block_a.hashables.previous));
        ledger.store.block_del (transaction, hash);
    }
    void receive_block (rai::receive_block const & block_a) override
    {
        auto hash (block_a.hash ());
        auto representative (ledger.representative (transaction, block_a.hashables.source));
        auto amount (ledger.amount (transaction, block_a.hashables.source));
        auto destination_account (ledger.account (transaction, hash));
        ledger.move_representation (transaction, ledger.representative (transaction, hash), representative, amount);
        ledger.change_latest (transaction, destination_account, block_a.hashables.previous, representative, ledger.balance (transaction, block_a.hashables.previous));
        ledger.store.block_del (transaction, hash);
        ledger.store.pending_put (transaction, block_a.hashables.source, {ledger.account (transaction, block_a.hashables.source), amount, destination_account});
    }
    void open_block (rai::open_block const & block_a) override
    {
        auto hash (block_a.hash ());
        auto representative (ledger.representative (transaction, block_a.hashables.source));
        auto amount (ledger.amount (transaction, block_a.hashables.source));
        auto destination_account (ledger.account (transaction, hash));
        ledger.move_representation (transaction, ledger.representative (transaction, hash), representative, amount);
        ledger.change_latest (transaction, destination_account, 0, representative, 0);
        ledger.store.block_del (transaction, hash);
        ledger.store.pending_put (transaction, block_a.hashables.source, {ledger.account (transaction, block_a.hashables.source), amount, destination_account});
    }
    void change_block (rai::change_block const & block_a) override
    {
        auto representative (ledger.representative (transaction, block_a.hashables.previous));
        auto account (ledger.account (transaction, block_a.hashables.previous));
        rai::frontier frontier;
        ledger.store.latest_get (transaction, account, frontier);
        ledger.move_representation (transaction, block_a.hashables.representative, representative, ledger.balance (transaction, block_a.hashables.previous));
        ledger.store.block_del (transaction, block_a.hash ());
        ledger.change_latest (transaction, account, block_a.hashables.previous, representative, frontier.balance);
    }
	MDB_txn * transaction;
    rai::ledger & ledger;
};
}

void amount_visitor::compute (rai::block_hash const & block_hash)
{
    auto block (store.block_get (transaction, block_hash));
	if (block != nullptr)
	{
		block->visit (*this);
	}
	else
	{
		if (block_hash == rai::genesis_account)
		{
			result = std::numeric_limits <rai::uint128_t>::max ();
		}
		else
		{
			assert (false);
			result = 0;
		}
	}
}

void balance_visitor::compute (rai::block_hash const & block_hash)
{
    auto block (store.block_get (transaction, block_hash));
    assert (block != nullptr);
    block->visit (*this);
}

// Balance for account containing hash
rai::uint128_t rai::ledger::balance (MDB_txn * transaction_a, rai::block_hash const & hash_a)
{
    balance_visitor visitor (transaction_a, store);
    visitor.compute (hash_a);
    return visitor.result;
}

// Balance for an account by account number
rai::uint128_t rai::ledger::account_balance (MDB_txn * transaction_a, rai::account const & account_a)
{
    rai::uint128_t result (0);
    rai::frontier frontier;
    auto none (store.latest_get (transaction_a, account_a, frontier));
    if (!none)
    {
        result = frontier.balance.number ();
    }
    return result;
}

rai::process_result rai::ledger::process (MDB_txn * transaction_a, rai::block const & block_a)
{
    ledger_processor processor (*this, transaction_a);
    block_a.visit (processor);
    return processor.result;
}

// Money supply for heuristically calculating vote percentages
rai::uint128_t rai::ledger::supply ()
{
    return std::numeric_limits <rai::uint128_t>::max ();
}

rai::account rai::ledger::representative (MDB_txn * transaction_a, rai::block_hash const & hash_a)
{
    auto result (representative_calculated (transaction_a, hash_a));
    //assert (result == representative_cached (hash_a));
    return result;
}

rai::account rai::ledger::representative_calculated (MDB_txn * transaction_a, rai::block_hash const & hash_a)
{
    representative_visitor visitor (transaction_a, store);
    visitor.compute (hash_a);
    return visitor.result;
}

rai::account rai::ledger::representative_cached (rai::block_hash const & hash_a)
{
    assert (false);
}

// Vote weight of an account
rai::uint128_t rai::ledger::weight (MDB_txn * transaction_a, rai::account const & account_a)
{
    return store.representation_get (transaction_a, account_a);
}

// Rollback blocks until `frontier_a' is the frontier block
void rai::ledger::rollback (MDB_txn * transaction_a, rai::block_hash const & frontier_a)
{
    auto account_l (account (transaction_a, frontier_a));
    rollback_visitor rollback (transaction_a, *this);
    rai::frontier frontier;
    do
    {
        auto latest_error (store.latest_get (transaction_a, account_l, frontier));
        assert (!latest_error);
        auto block (store.block_get (transaction_a, frontier.hash));
        block->visit (rollback);
    // Continue rolling back until this block is the frontier
    } while (frontier.hash != frontier_a);
}

// Return account containing hash
rai::account rai::ledger::account (MDB_txn * transaction_a, rai::block_hash const & hash_a)
{
    account_visitor account (transaction_a, store);
    account.compute (hash_a);
    return account.result;
}

// Return amount decrease or increase for block
rai::uint128_t rai::ledger::amount (MDB_txn * transaction_a, rai::block_hash const & hash_a)
{
    amount_visitor amount (transaction_a, store);
    amount.compute (hash_a);
    return amount.result;
}

void rai::ledger::move_representation (MDB_txn * transaction_a, rai::account const & source_a, rai::account const & destination_a, rai::uint128_t const & amount_a)
{
    auto source_previous (store.representation_get (transaction_a, source_a));
    assert (source_previous >= amount_a);
    store.representation_put (transaction_a, source_a, source_previous - amount_a);
    auto destination_previous (store.representation_get (transaction_a, destination_a));
    store.representation_put (transaction_a, destination_a, destination_previous + amount_a);
}

// Return latest block for account
rai::block_hash rai::ledger::latest (MDB_txn * transaction_a, rai::account const & account_a)
{
    rai::frontier frontier;
    auto latest_error (store.latest_get (transaction_a, account_a, frontier));
	return latest_error ? 0 : frontier.hash;
}

// Return latest root for account, account number of there are no blocks for this account.
rai::block_hash rai::ledger::latest_root (MDB_txn * transaction_a, rai::account const & account_a)
{
    rai::frontier frontier;
    auto latest_error (store.latest_get (transaction_a, account_a, frontier));
    rai::block_hash result;
    if (latest_error)
    {
        result = account_a;
    }
    else
    {
        result = frontier.hash;
    }
    return result;
}

rai::checksum rai::ledger::checksum (MDB_txn * transaction_a, rai::account const & begin_a, rai::account const & end_a)
{
    rai::checksum result;
    auto error (store.checksum_get (transaction_a, 0, 0, result));
    assert (!error);
    return result;
}

void rai::ledger::dump_account_chain (rai::account const & account_a)
{
	rai::transaction transaction (store.environment, nullptr, false);
    auto hash (latest (transaction, account_a));
    while (!hash.is_zero ())
    {
        auto block (store.block_get (transaction, hash));
        assert (block != nullptr);
        std::cerr << hash.to_string () << std::endl;
        hash = block->previous ();
    }
}

void rai::ledger::checksum_update (MDB_txn * transaction_a, rai::block_hash const & hash_a)
{
	rai::checksum value;
    auto error (store.checksum_get (transaction_a, 0, 0, value));
    assert (!error);
    value ^= hash_a;
    store.checksum_put (transaction_a, 0, 0, value);
}

void rai::ledger::change_latest (MDB_txn * transaction_a, rai::account const & account_a, rai::block_hash const & hash_a, rai::account const & representative_a, rai::amount const & balance_a)
{
    rai::frontier frontier;
    auto exists (!store.latest_get (transaction_a, account_a, frontier));
    if (exists)
    {
        checksum_update (transaction_a, frontier.hash);
    }
    if (!hash_a.is_zero())
    {
        frontier.hash = hash_a;
        frontier.representative = representative_a;
        frontier.balance = balance_a;
        frontier.time = store.now ();
        store.latest_put (transaction_a, account_a, frontier);
        checksum_update (transaction_a, hash_a);
    }
    else
    {
        store.latest_del (transaction_a, account_a);
    }
}

std::unique_ptr <rai::block> rai::ledger::successor (MDB_txn * transaction_a, rai::block_hash const & block_a)
{
    assert (store.block_exists (transaction_a, block_a));
    assert (latest (transaction_a, account (transaction_a, block_a)) != block_a);
	auto successor (store.block_successor (transaction_a, block_a));
	assert (!successor.is_zero ());
	auto result (store.block_get (transaction_a, successor));
	assert (result != nullptr);
    return result;
}

void ledger_processor::change_block (rai::change_block const & block_a)
{
    rai::uint256_union message (block_a.hash ());
    auto existing (ledger.store.block_exists (transaction, message));
    result = existing ? rai::process_result::old : rai::process_result::progress; // Have we seen this block before? (Harmless)
    if (result == rai::process_result::progress)
    {
        auto previous (ledger.store.block_exists (transaction, block_a.hashables.previous));
        result = previous ? rai::process_result::progress : rai::process_result::gap_previous;  // Have we seen the previous block already? (Harmless)
        if (result == rai::process_result::progress)
        {
            auto account (ledger.account (transaction, block_a.hashables.previous));
            rai::frontier frontier;
            auto latest_error (ledger.store.latest_get (transaction, account, frontier));
            assert (!latest_error);
            result = validate_message (account, message, block_a.signature) ? rai::process_result::bad_signature : rai::process_result::progress; // Is this block signed correctly (Malformed)
            if (result == rai::process_result::progress)
            {
                result = frontier.hash == block_a.hashables.previous ? rai::process_result::progress : rai::process_result::fork; // Is the previous block the latest (Malicious)
                if (result == rai::process_result::progress)
                {
					ledger.move_representation (transaction, frontier.representative, block_a.hashables.representative, ledger.balance (transaction, block_a.hashables.previous));
					ledger.store.block_put (transaction, message, block_a);
					ledger.change_latest (transaction, account, message, block_a.hashables.representative, frontier.balance);
                    ledger.change_observer (block_a, account);
                }
            }
        }
    }
}

void ledger_processor::send_block (rai::send_block const & block_a)
{
    rai::uint256_union message (block_a.hash ());
    auto existing (ledger.store.block_exists (transaction, message));
    result = existing ? rai::process_result::old : rai::process_result::progress; // Have we seen this block before? (Harmless)
    if (result == rai::process_result::progress)
    {
        auto previous (ledger.store.block_exists (transaction, block_a.hashables.previous));
        result = previous ? rai::process_result::progress : rai::process_result::gap_previous; // Have we seen the previous block already? (Harmless)
        if (result == rai::process_result::progress)
        {
            auto account (ledger.account (transaction, block_a.hashables.previous));
            result = validate_message (account, message, block_a.signature) ? rai::process_result::bad_signature : rai::process_result::progress; // Is this block signed correctly (Malformed)
            if (result == rai::process_result::progress)
            {
                rai::frontier frontier;
                auto latest_error (ledger.store.latest_get (transaction, account, frontier));
                assert (!latest_error);
                result = frontier.balance.number () >= block_a.hashables.balance.number () ? rai::process_result::progress : rai::process_result::overspend; // Is this trying to spend more than they have (Malicious)
                if (result == rai::process_result::progress)
                {
                    result = frontier.hash == block_a.hashables.previous ? rai::process_result::progress : rai::process_result::fork;
                    if (result == rai::process_result::progress)
                    {
						ledger.store.block_put (transaction, message, block_a);
						ledger.change_latest (transaction, account, message, frontier.representative, block_a.hashables.balance);
						ledger.store.pending_put (transaction, message, {account, frontier.balance.number () - block_a.hashables.balance.number (), block_a.hashables.destination});
						ledger.send_observer (block_a, account);
                    }
                }
            }
        }
    }
}

void ledger_processor::receive_block (rai::receive_block const & block_a)
{
    auto hash (block_a.hash ());
    auto existing (ledger.store.block_exists (transaction, hash));
    result = existing ? rai::process_result::old : rai::process_result::progress; // Have we seen this block already?  (Harmless)
    if (result == rai::process_result::progress)
    {
        auto source_missing (!ledger.store.block_exists (transaction, block_a.hashables.source));
        result = source_missing ? rai::process_result::gap_source : rai::process_result::progress; // Have we seen the source block already? (Harmless)
        if (result == rai::process_result::progress)
        {
            rai::receivable receivable;
            result = ledger.store.pending_get (transaction, block_a.hashables.source, receivable) ? rai::process_result::unreceivable : rai::process_result::progress; // Has this source already been received (Malformed)
            if (result == rai::process_result::progress)
            {
                result = rai::validate_message (receivable.destination, hash, block_a.signature) ? rai::process_result::bad_signature : rai::process_result::progress; // Is the signature valid (Malformed)
                if (result == rai::process_result::progress)
                {
                    rai::frontier frontier;
                    result = ledger.store.latest_get (transaction, receivable.destination, frontier) ? rai::process_result::gap_previous : rai::process_result::progress;  //Have we seen the previous block? No entries for account at all (Harmless)
                    if (result == rai::process_result::progress)
                    {
                        result = frontier.hash == block_a.hashables.previous ? rai::process_result::progress : rai::process_result::gap_previous; // Block doesn't immediately follow latest block (Harmless)
                        if (result == rai::process_result::progress)
                        {
                            auto new_balance (frontier.balance.number () + receivable.amount.number ());
                            rai::frontier source_frontier;
                            auto error (ledger.store.latest_get (transaction, receivable.source, source_frontier));
                            assert (!error);
							ledger.store.pending_del (transaction, block_a.hashables.source);
							ledger.store.block_put (transaction, hash, block_a);
							ledger.change_latest (transaction, receivable.destination, hash, frontier.representative, new_balance);
							ledger.move_representation (transaction, source_frontier.representative, frontier.representative, receivable.amount.number ());
							ledger.receive_observer (block_a, receivable.destination);
                        }
                        else
                        {
                            result = ledger.store.block_exists (transaction, block_a.hashables.previous) ? rai::process_result::fork : rai::process_result::gap_previous; // If we have the block but it's not the latest we have a signed fork (Malicious)
                        }
                    }
                }
            }
        }
    }
}

void ledger_processor::open_block (rai::open_block const & block_a)
{
    auto hash (block_a.hash ());
    auto existing (ledger.store.block_exists (transaction, hash));
    result = existing ? rai::process_result::old : rai::process_result::progress; // Have we seen this block already? (Harmless)
    if (result == rai::process_result::progress)
    {
        auto source_missing (!ledger.store.block_exists (transaction, block_a.hashables.source));
        result = source_missing ? rai::process_result::gap_source : rai::process_result::progress; // Have we seen the source block? (Harmless)
        if (result == rai::process_result::progress)
        {
            rai::receivable receivable;
            result = ledger.store.pending_get (transaction, block_a.hashables.source, receivable) ? rai::process_result::unreceivable : rai::process_result::progress; // Has this source already been received (Malformed)
            if (result == rai::process_result::progress)
            {
                result = receivable.destination == block_a.hashables.account ? rai::process_result::progress : rai::process_result::account_mismatch;
                if (result == rai::process_result::progress)
                {
                    result = rai::validate_message (receivable.destination, hash, block_a.signature) ? rai::process_result::bad_signature : rai::process_result::progress; // Is the signature valid (Malformed)
                    if (result == rai::process_result::progress)
                    {
                        rai::frontier frontier;
                        result = ledger.store.latest_get (transaction, receivable.destination, frontier) ? rai::process_result::progress : rai::process_result::fork; // Has this account already been opened? (Malicious)
                        if (result == rai::process_result::progress)
                        {
                            rai::frontier source_frontier;
                            auto error (ledger.store.latest_get (transaction, receivable.source, source_frontier));
                            assert (!error);
							ledger.store.pending_del (transaction, block_a.hashables.source);
							ledger.store.block_put (transaction, hash, block_a);
							ledger.change_latest (transaction, receivable.destination, hash, block_a.hashables.representative, receivable.amount.number ());
							ledger.move_representation (transaction, source_frontier.representative, block_a.hashables.representative, receivable.amount.number ());
							ledger.open_observer (block_a, receivable.destination);
                        }
                    }
                }
            }
        }
    }
}

ledger_processor::ledger_processor (rai::ledger & ledger_a, MDB_txn * transaction_a) :
ledger (ledger_a),
transaction (transaction_a),
result (rai::process_result::progress)
{
}

rai::vote::vote (bool & error_a, rai::stream & stream_a, rai::block_type type_a)
{
	if (!error_a)
	{
		error_a = rai::read (stream_a, account.bytes);
		if (!error_a)
		{
			error_a = rai::read (stream_a, signature.bytes);
			if (!error_a)
			{
				error_a = rai::read (stream_a, sequence);
				if (!error_a)
				{
					block = rai::deserialize_block (stream_a, type_a);
					error_a = block == nullptr;
				}
			}
		}
	}
}

rai::vote::vote (rai::account const & account_a, rai::private_key const & prv_a, uint64_t sequence_a, std::unique_ptr <rai::block> block_a) :
sequence (sequence_a),
block (std::move (block_a)),
account (account_a),
signature (rai::sign_message (prv_a, account_a, hash ()))
{
}

rai::uint256_union rai::vote::hash () const
{
    rai::uint256_union result;
    blake2b_state hash;
	blake2b_init (&hash, sizeof (result.bytes));
    blake2b_update (&hash, block->hash ().bytes.data (), sizeof (result.bytes));
    union {
        uint64_t qword;
        std::array <uint8_t, 8> bytes;
    };
    qword = sequence;
    blake2b_update (&hash, bytes.data (), sizeof (bytes));
    blake2b_final (&hash, result.bytes.data (), sizeof (result.bytes));
    return result;
}

rai::genesis::genesis () :
open (genesis_account, genesis_account, genesis_account, nullptr)
{
}

void rai::genesis::initialize (MDB_txn * transaction_a, rai::block_store & store_a) const
{
	assert (store_a.latest_begin (transaction_a) == store_a.latest_end ());
	store_a.block_put (transaction_a, open.hash (), open);
	store_a.latest_put (transaction_a, genesis_account, {open.hash (), open.hashables.representative, std::numeric_limits <rai::uint128_t>::max (), store_a.now ()});
	store_a.representation_put (transaction_a, genesis_account, std::numeric_limits <rai::uint128_t>::max ());
	store_a.checksum_put (transaction_a, 0, 0, hash ());
}

rai::block_hash rai::genesis::hash () const
{
    return open.hash ();
}