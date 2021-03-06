#include <bitcoin/transaction_pool.hpp>

#include <bitcoin/error.hpp>
#include <bitcoin/transaction.hpp>
#include <bitcoin/validate.hpp>
#include <bitcoin/utility/assert.hpp>

namespace libbitcoin {

using std::placeholders::_1;
using std::placeholders::_2;
using std::placeholders::_3;
using std::placeholders::_4;

transaction_pool::transaction_pool(
    async_service& service, blockchain& chain)
  : strand_(service.get_service()), chain_(chain), pool_(2000)
{
}
void transaction_pool::start()
{
    chain_.subscribe_reorganize(
        strand_.wrap(std::bind(&transaction_pool::reorganize,
            this, _1, _2, _3, _4)));
}

void transaction_pool::store(const message::transaction& stored_transaction,
    confirm_handler handle_confirm, store_handler handle_store)
{
    strand_.post(
        std::bind(&transaction_pool::do_store,
            this, stored_transaction, handle_confirm, handle_store));
}
void transaction_pool::do_store(
    const message::transaction& stored_transaction,
    confirm_handler handle_confirm, store_handler handle_store)
{
    transaction_entry_info new_tx_entry{
        hash_transaction(stored_transaction),
        stored_transaction,
        handle_confirm};

    validate_transaction_ptr validate =
        std::make_shared<validate_transaction>(
            chain_, stored_transaction, pool_, strand_);
    validate->start(strand_.wrap(std::bind(
        &transaction_pool::handle_delegate,
            this, _1, _2, new_tx_entry, handle_store)));
}

void transaction_pool::handle_delegate(
    const std::error_code& ec, const index_list& unconfirmed,
    const transaction_entry_info& tx_entry, store_handler handle_store)
{
    if (ec == error::input_not_found)
    {
        BITCOIN_ASSERT(unconfirmed.size() == 1);
        BITCOIN_ASSERT(unconfirmed[0] < tx_entry.tx.inputs.size());
        handle_store(ec, unconfirmed);
    }
    else if (ec)
    {
        BITCOIN_ASSERT(unconfirmed.empty());
        handle_store(ec, index_list());
    }
    // Re-check as another transaction might've been added in the interim
    else if (tx_exists(tx_entry.hash))
    {
        handle_store(error::duplicate, index_list());
    }
    else
    {
        pool_.push_back(tx_entry);
        handle_store(std::error_code(), unconfirmed);
    }
}

bool transaction_pool::tx_exists(const hash_digest& tx_hash)
{
    for (const transaction_entry_info& entry: pool_)
        if (entry.hash == tx_hash)
            return true;
    return false;
}

void transaction_pool::fetch(const hash_digest& transaction_hash,
    fetch_handler handle_fetch)
{
    strand_.post(
        [this, transaction_hash, handle_fetch]()
        {
            for (const transaction_entry_info& entry: pool_)
                if (entry.hash == transaction_hash)
                {
                    handle_fetch(std::error_code(), entry.tx);
                    return;
                }
            handle_fetch(error::not_found, message::transaction());
        });
}

void transaction_pool::exists(const hash_digest& transaction_hash,
    exists_handler handle_exists)
{
    strand_.post(
        [this, transaction_hash, handle_exists]()
        {
            handle_exists(tx_exists(transaction_hash));
        });
}

void transaction_pool::reorganize(const std::error_code& ec,
    size_t fork_point,
    const blockchain::block_list& new_blocks,
    const blockchain::block_list& replaced_blocks)
{
    if (!replaced_blocks.empty())
        resubmit_all();
    else
        takeout_confirmed(new_blocks);
    // new blocks come in - remove txs in new
    // old blocks taken out - resubmit txs in old
    chain_.subscribe_reorganize(
        strand_.wrap(std::bind(&transaction_pool::reorganize,
            this, _1, _2, _3, _4)));
}

void handle_resubmit(const std::error_code& ec,
    transaction_pool::confirm_handler handle_confirm)
{
    if (ec)
        handle_confirm(ec);
}
void transaction_pool::resubmit_all()
{
    for (const transaction_entry_info& entry: pool_)
        store(entry.tx, entry.handle_confirm,
            std::bind(handle_resubmit, _1, entry.handle_confirm));
    pool_.clear();
}

void transaction_pool::takeout_confirmed(
    const blockchain::block_list& new_blocks)
{
    for (auto new_block: new_blocks)
        for (const message::transaction& new_tx: new_block->transactions)
            try_delete(hash_transaction(new_tx));
}
void transaction_pool::try_delete(const hash_digest& tx_hash)
{
    for (auto it = pool_.begin(); it != pool_.end(); ++it)
        if (it->hash == tx_hash)
        {
            auto handle_confirm = it->handle_confirm;
            pool_.erase(it);
            handle_confirm(std::error_code());
            return;
        }
}

} // namespace libbitcoin

