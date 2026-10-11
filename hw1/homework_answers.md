# Homework Answers

## 1. Bernie vs. a 150 Mbps transmission line

Using 1 GB = \(10^9\) bytes, Bernie carries:

\[
3 \times 7\text{ GB} \times 8 = 168\text{ Gb}
\]

At 18 km/h = 0.005 km/s, the travel time for distance \(d\) is \(d/0.005\) s. Bernie is faster when

\[
\frac{168\times10^9}{d/0.005} > 150\times10^6
\quad\Rightarrow\quad d < 5.6\text{ km}.
\]

Thus, Bernie has the higher data rate for **0 < d < 5.6 km** (one-way distance).

- (i) If Bernie’s speed doubles: **0 < d < 11.2 km**.
- (ii) If each tape’s capacity doubles: **0 < d < 11.2 km**.
- (iii) If the line rate doubles to 300 Mbps: **0 < d < 2.8 km**.

## 2. Layered protocols

Two reasons:

1. They divide a complex networking task into manageable modules.
2. A layer can be changed or replaced without redesigning the other layers.

Disadvantage: layering can add overhead and reduce performance.

## 3. Retransmissions

Each attempt uses one transmission; with probability \(p\), another attempt is needed. Assuming independent errors and \(p<1\):

\[
E[N]=1+pE[N]\quad\Rightarrow\quad E[N] = \frac{1}{1-p}.
\]

The mean number of transmissions is **\(1/(1-p)\)**.

## 4. TCP vs. UDP

**TCP** is connection-oriented and provides reliable, ordered delivery. **UDP** is connectionless and provides no delivery, ordering, or retransmission guarantees.

## 5. Length of one bit in 802.3

Propagation speed:

\[
v=\frac{2}{3}(3\times10^8)=2\times10^8\text{ m/s}
\]

At 10 Mbps, one bit lasts \(1/(10\times10^6)\) s, so

\[
L=v\times\frac{1}{10\times10^6}=20\text{ m}.
\]

One bit was **20 meters** long.

## 6. International standards

- Advantage: they ensure interoperability between equipment from different countries and vendors.
- Disadvantage: reaching agreement can be slow, and the resulting standards may be difficult to update.

## 7. Common OSI and TCP/IP layers

Both models have **application**, **transport**, and **network** layers (called **Internet** in TCP/IP).

In the four-layer TCP/IP model, the **network access** layer covers OSI’s data link and physical layers.
