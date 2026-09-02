# Junocash Documentation

Junocash is a privacy-focused cryptocurrency using the Orchard shielded protocol.

## Components

- **junocashd** – the full node and wallet backend
- **junocash-cli** – command-line interface that communicates with junocashd

## Starting and Stopping the Node

### Interactive Mode

Run `junocashd` without arguments to launch in interactive mode with a visual dashboard:

```
┌────────────────────────────────────────────────────────────────────────┐
│                               Junocash                                │
│                         Privacy Money for All                          │
│                       v0.9.4 - mainnet - RandomX                       │
└────────────────────────────────────────────────────────────────────────┘

┌──────────────────────────── NETWORK STATUS ────────────────────────────┐
│ Status                                                  ● SYNCHRONIZED │
│ Block Height                                                     31564 │
│ Network Difficulty                                        42621.987786 │
│ Next Upgrade                                            None scheduled │
│ Connections                                                          8 │
│ Network Hash                                                6.308 MH/s │
└────────────────────────────────────────────────────────────────────────┘

┌──────────────────────────────── WALLET ────────────────────────────────┐
│ Mature Balance                                               0.00 JUNO │
│ Immature Balance                                             0.00 JUNO │
└────────────────────────────────────────────────────────────────────────┘

┌──────────────────────────────── MINING ────────────────────────────────┐
│ Status                                                      ○ INACTIVE │
└────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────── CONTROLS ───────────────────────────────┐
│                            [M] Mining: OFF                             │
└────────────────────────────────────────────────────────────────────────┘
```

When mining is enabled, the display expands:

```
┌──────────────────────────────── MINING ────────────────────────────────┐
│ Status                                            ● ACTIVE - 1 threads │
│ Block Reward                                                     12.50 │
└────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────── CONTROLS ───────────────────────────────┐
│           [M] Mining: ON  [T] Threads: 1  [D] Donations: OFF           │
└────────────────────────────────────────────────────────────────────────┘
```

**Dashboard controls:**
- **M** – Toggle mining on/off
- **T** – Set number of mining threads (use -1 for maximum)
- **D** – Toggle developer donations
- **Ctrl+C** (twice) – Exit the node

### Background Mode

```bash
# Start as daemon (logs written to debug.log in data directory)
junocashd -daemon

# Stop the daemon
junocash-cli stop
```

## Data Directories

| OS      | Location                                      |
|---------|-----------------------------------------------|
| Windows | `%APPDATA%\JunoCash`                          |
| Linux   | `~/.junocash`                                 |
| macOS   | `~/Library/Application Support/JunoCash`      |

These directories contain the blockchain, wallet files, configuration (`junocash.conf`), and logs. Use `-datadir` to specify a custom location.

**Important:** Your wallet is stored in `wallet.dat`. Never delete this file. If you need to copy it, stop junocashd first.

## Backup Your Wallet

View your 24-word recovery seed phrase:

```bash
junocash-cli z_getseedphrase
```

Write down these words and store them securely. Anyone with this phrase can access your funds.

## Mining

Mining rewards go to a transparent address and require 100 confirmations to mature.

```bash
# Start mining with 2 threads
junocash-cli setgenerate true 2

# Stop mining
junocash-cli setgenerate false

# Check current block height
junocash-cli getblockcount

# Check transparent balance
junocash-cli getbalance
```

**Mature vs Immature balance:** Coins less than 100 blocks old are immature and cannot be spent yet.

## Transaction Flow

Junocash uses a privacy-preserving transaction flow:

1. **Mine** → rewards go to a transparent address
2. **Shield** → move coinbase to an Orchard shielded address
3. **Send** → transact privately between Orchard addresses

## Wallet Operations

### Generate a Shielded Address

```bash
# Create a new account (first time only)
junocash-cli z_getnewaccount

# Get your unified address (starts with j1...)
junocash-cli z_getaddressforaccount 0
```

### Shield Coinbase (Mining Rewards)

You must shield mining rewards before spending them:

```bash
junocash-cli z_shieldcoinbase "*" "j1YourAddressHere..."
```

This returns an operation ID. If you have many coinbase transactions, they will be batched (~50 per operation).

### Check Balances

```bash
# Shielded balance for a specific address
junocash-cli z_getbalance "j1YourAddressHere..."

# Total balance (transparent + shielded)
junocash-cli z_gettotalbalance

# List unspent shielded notes
junocash-cli z_listunspent
```

### Send Transactions

```bash
# Send to one recipient
junocash-cli z_send "j1FromAddress..." "j1ToAddress..." 1.5 "Optional memo"

# Send to multiple recipients
junocash-cli z_sendmany "j1FromAddress..." '[
  {"address": "j1Recipient1...", "amount": 1.0},
  {"address": "j1Recipient2...", "amount": 2.0, "memo": "Payment"}
]'
```

### View Transactions

```bash
# View transaction details
junocash-cli z_viewtransaction "txid..."

# List recent transactions
junocash-cli listtransactions
```

## Checking Operation Status

Shielded operations (shielding, sending) run asynchronously and return an operation ID.

```bash
# Check status of all operations
junocash-cli z_getoperationstatus

# Check specific operation
junocash-cli z_getoperationstatus '["opid-xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"]'

# Get results and clear from memory
junocash-cli z_getoperationresult

# List all operation IDs
junocash-cli z_listoperationids
```

**Status values:**
- `queued` – waiting to execute
- `executing` – currently running
- `success` – completed successfully
- `failed` – check the `error` field for details

A successful operation returns:
```json
{
  "id": "opid-xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx",
  "status": "success",
  "creation_time": 1234567890,
  "result": { "txid": "abc123..." },
  "execution_secs": 5.123
}
```

## Troubleshooting

**"Insufficient funds"**
- Ensure coinbase transactions have 100+ confirmations
- Verify balance with `z_getbalance`

**"Address must contain an Orchard receiver"**
- Use unified addresses from `z_getaddressforaccount`, not legacy addresses

**Operation stuck in "executing"**
- Wait for network confirmation
- Monitor with `z_getoperationstatus`

## Tips

- Fees are calculated automatically using ZIP-317
- Shielded transactions hide amounts from everyone except sender and recipient
- Back up your seed phrase immediately after creating a wallet

## Advanced: Manual Mining Address

By default, junocashd manages mining addresses automatically. If you need to send mining rewards to a specific fixed address (for example, when using external mining software), you can generate a mining address:

```bash
junocash-cli t_getminingaddress
```

Then add it to your `junocash.conf`:

```
mineraddress=t1YourAddressHere...
```

This is rarely necessary for normal use.
