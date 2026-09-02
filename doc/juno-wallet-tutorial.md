# Junocash Wallet Tutorial

This guide covers the basic wallet operations in Junocash: mining, shielding coinbase, generating addresses, and sending transactions.

## Overview

Junocash uses a privacy-preserving transaction flow:

1. **Mine** to a transparent address (automatic)
2. **Shield** the coinbase to an Orchard shielded address
3. **Send** between Orchard addresses privately

All shielded transactions use the Orchard protocol. Sapling and Sprout are not supported.

## 1. Mining

When you start mining, the node automatically generates a transparent address if needed:

```bash
# Start mining with 2 threads
junocash-cli setgenerate true 2

# Stop mining
junocash-cli setgenerate false

# Check block count
junocash-cli getblockcount

# Check transparent balance
junocash-cli getbalance
```

### Manual Mining Address (NOT recommended)

Please skip this section it is unnecessary.

If you want to specify a mining address in your config file, use `t_getminingaddress`:

```bash
junocash-cli t_getminingaddress
```

Then add to junocashd.conf:
```
mineraddress=t1YourAddressHere...
```

This is rarely needed since the node handles address generation automatically.

## 2. Shielding Coinbase Mining rewards

(Coinbase here means a mining transaction which mints the block reward, it's not that exchange in the US).

Mining rewards go to a transparent address. You must shield them to an Orchard address before spending. Coinbase requires 100 confirmations to mature.

### Get Your Orchard Address

```bash
# Create a new account (first time only)
junocash-cli z_getnewaccount

# Get your unified address (can reuse same address)
junocash-cli z_getaddressforaccount 0
```

This returns your unified address (starts with `j1...`).

### Shield Your Coinbase

```bash
# Shield from all transparent addresses to your unified address
junocash-cli z_shieldcoinbase "*" "j1YourAddressHere..."
```

If you have a lot of coinbase transactions it will limit to around 50 per time.

This returns an operation ID and shows how many UTXOs are being shielded.

### Check Operation Status

```bash
junocash-cli z_getoperationstatus
```

Wait for status to show "success" and for the transaction to be confirmed.

## 3. Checking Balance

```bash
# Check shielded balance for your address
junocash-cli z_getbalance "j1YourAddressHere..."

# Check total balance (transparent and shielded)
junocash-cli z_gettotalbalance

# List unspent shielded notes
junocash-cli z_listunspent
```

## 4. Sending Transactions

### Send to One Recipient

```bash
# Send 1.5 JUNO with a memo
junocash-cli z_send "j1FromAddress..." "j1ToAddress..." 1.5 "Hello!"

# Send without memo
junocash-cli z_send "j1FromAddress..." "j1ToAddress..." 2.0
```

### Send to Multiple Recipients

```bash
junocash-cli z_sendmany "j1FromAddress..." '[
  {"address": "j1Recipient1...", "amount": 1.0},
  {"address": "j1Recipient2...", "amount": 2.0, "memo": "Payment"}
]'
```

Both commands return an operation ID. Check status with `z_getoperationstatus`.

## 5. Viewing Transactions

```bash
# View details of a transaction
junocash-cli z_viewtransaction "txid..."

# List recent transactions
junocash-cli listtransactions
```

## 6. Backup Your Wallet

View your 24-word recovery seed phrase:

```bash
junocash-cli z_getseedphrase
```

Write down these words and store them safely. Anyone with this phrase can access all your funds.

## Tips

- **Coinbase Maturity**: Mining rewards need 100 confirmations before shielding
- **Fees**: Calculated automatically using ZIP-317
- **Privacy**: Shielded transactions hide amounts from everyone except sender and recipient
- **Backups**: Write down your seed phrase with `z_getseedphrase` and store it safely

## Troubleshooting

**"Insufficient funds"**
- Check that coinbases have 100+ confirmations
- Verify balance with `z_getbalance`

**"Address must contain an Orchard receiver"**
- Use unified addresses from `z_getaddressforaccount`, not legacy addresses

**Operation stuck in "executing"**
- The transaction needs to be confirmed on the network
- Check `z_getoperationstatus` for updates
